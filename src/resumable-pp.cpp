#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;

//------------------------------------------------------------------------------
// The following code is injected at the beginning of the preprocessed input.

const char injected[] = R"-(

struct __yield_t
{
  constexpr __yield_t() {}
  template <class T> operator T() const;
  template <class T> __yield_t operator&(const T&) const;
};

constexpr __yield_t __yield;

struct __from_t
{
  constexpr __from_t() {}
  template <class T> operator T() const;
  template <class T> __from_t operator&(const T&) const;
};

constexpr __from_t __from;

struct __lambda_this_t
{
  constexpr __lambda_this_t() {}
  template <class T> operator T() const;
  __lambda_this_t operator*() const;
  void operator()() const;
};

constexpr __lambda_this_t __lambda_this;

#define resumable __attribute__((__annotate__("resumable"))) mutable
#define yield 0 ? throw __yield : throw
#define from __from&
#define lambda_this __lambda_this

)-";

//------------------------------------------------------------------------------
// Class to inject code at the beginning of the input.

class code_injector : public PPCallbacks
{
public:
  explicit code_injector(Preprocessor& pp)
    : preprocessor_(pp)
  {
  }

  void FileChanged(SourceLocation loc, FileChangeReason reason, SrcMgr::CharacteristicKind type, FileID prev_fid)
  {
    SourceManager &source_mgr = preprocessor_.getSourceManager();
    const FileEntry* file_entry = source_mgr.getFileEntryForID(source_mgr.getFileID(source_mgr.getExpansionLoc(loc)));
    if (!file_entry)
      return;

    if (reason != EnterFile)
      return;

    if (source_mgr.getFileID(source_mgr.getFileLoc(loc)) == source_mgr.getMainFileID())
    {
      auto buf = llvm::MemoryBuffer::getMemBuffer(injected, "resumable-pp-injected");
      loc = source_mgr.getFileLoc(loc);
      preprocessor_.EnterSourceFile(source_mgr.createFileID(buf, SrcMgr::C_User, 0, 0, loc), nullptr, loc);
    }
  }

private:
  Preprocessor& preprocessor_;
};

//------------------------------------------------------------------------------
// Helper function to detect whether an AST node is a "yield" keyword.

Expr* IsYieldKeyword(Stmt* stmt)
{
  if (ConditionalOperator* op = dyn_cast<ConditionalOperator>(stmt))
    if (op->getLocStart().isMacroID())
      if (CXXThrowExpr* true_throw_expr = dyn_cast<CXXThrowExpr>(op->getTrueExpr()))
        if (CXXConstructExpr* construct_expr = dyn_cast<CXXConstructExpr>(true_throw_expr->getSubExpr()))
          if (construct_expr->getType().getAsString() == "struct __yield_t")
            if (CXXThrowExpr* false_throw_expr = dyn_cast<CXXThrowExpr>(op->getFalseExpr()))
              return false_throw_expr->getSubExpr();
  return nullptr;
}

//------------------------------------------------------------------------------
// Helper function to detect whether an AST node is a "from" keyword.

Expr* IsFromKeyword(Stmt* stmt)
{
  if (CXXConstructExpr* construct_expr = dyn_cast<CXXConstructExpr>(stmt))
    if (construct_expr->getNumArgs() == 1)
      if (MaterializeTemporaryExpr* temp = dyn_cast<MaterializeTemporaryExpr>(construct_expr->getArg(0)))
        if (CXXOperatorCallExpr* call_expr = dyn_cast<CXXOperatorCallExpr>(temp->getTemporary()))
          if (call_expr->getNumArgs() == 2)
            if (call_expr->getArg(0)->getType().getAsString() == "const struct __from_t")
              return call_expr->getArg(1);
  if (ImplicitCastExpr* cast_expr = dyn_cast<ImplicitCastExpr>(stmt))
    if (CXXMemberCallExpr* call_expr = dyn_cast<CXXMemberCallExpr>(cast_expr->getSubExpr()))
      if (MemberExpr* member_expr = dyn_cast<MemberExpr>(call_expr->getCallee()))
        if (ImplicitCastExpr* cast_expr_2 = dyn_cast<ImplicitCastExpr>(member_expr->getBase()))
          if (CXXOperatorCallExpr* call_expr = dyn_cast<CXXOperatorCallExpr>(cast_expr_2->getSubExpr()))
            if (call_expr->getNumArgs() == 2)
              if (call_expr->getArg(0)->getType().getAsString() == "const struct __from_t")
                return call_expr->getArg(1);
  return nullptr;
}

//------------------------------------------------------------------------------
// Class to determine whether a lambda is a recursive lambda based on:
// - Being explicitly marked "resumable"
// - Using "yield", "yield from" or "return from" in the body.

class resumable_lambda_detector :
  public RecursiveASTVisitor<resumable_lambda_detector>
{
public:
  bool IsResumable(LambdaExpr* expr)
  {
    AnnotateAttr* attr = expr->getCallOperator()->getAttr<AnnotateAttr>();
    if (!attr || attr->getAnnotation() != "resumable")
      return true;

    is_resumable_ = false;
    nesting_level_ = 0;
    TraverseCompoundStmt(expr->getBody());
    return is_resumable_;
  }

  bool TraverseLambdaExpr(LambdaExpr* expr)
  {
    ++nesting_level_;
    TraverseCompoundStmt(expr->getBody());
    --nesting_level_;
    return true;
  }

  bool VisitConditionalOperator(ConditionalOperator* op)
  {
    if (nesting_level_ == 0 && IsYieldKeyword(op))
      is_resumable_ = true;
    return true;
  }

  bool VisitReturnStmt(ReturnStmt* stmt)
  {
    if (nesting_level_ == 0 && IsFromKeyword(stmt->getRetValue()))
      is_resumable_ = true;
    return true;
  }

private:
  bool is_resumable_ = false;
  int nesting_level_ = 0;
};

//------------------------------------------------------------------------------
// Class to visit the lambda body and build a database of all local members.

class resumable_lambda_locals:
  public RecursiveASTVisitor<resumable_lambda_locals>
{
public:
  typedef std::vector<int> scope_path;
  typedef std::map<scope_path, ValueDecl*>::iterator iterator;
  typedef std::map<scope_path, ValueDecl*>::reverse_iterator reverse_iterator;

  void Build(LambdaExpr* expr)
  {
    curr_scope_path_.clear();
    next_scope_id_ = 0;
    next_yield_id_ = 1;
    curr_scope_yield_id_ = 0;
    TraverseCompoundStmt(expr->getBody());
  }

  iterator begin()
  {
    return scope_to_decl_.begin();
  }

  iterator end()
  {
    return scope_to_decl_.end();
  }

  reverse_iterator rbegin()
  {
    return scope_to_decl_.rbegin();
  }

  reverse_iterator rend()
  {
    return scope_to_decl_.rend();
  }

  std::string getPathAsString(ValueDecl* decl)
  {
    auto iter = decl_to_scope_.find(decl);
    if (iter != decl_to_scope_.end())
    {
      std::string s;
      for (int scope: iter->second)
        s += "__s" + std::to_string(scope) + ".";
      s += decl->getDeclName().getAsString();
      return s;
    }
    return std::string();
  }

  int getYieldId(ValueDecl* decl)
  {
    auto iter = ptr_to_yield_.find(decl);
    return iter != ptr_to_yield_.end() ? iter->second : -1;
  }

  int getYieldId(Stmt* stmt)
  {
    auto iter = ptr_to_yield_.find(stmt);
    return iter != ptr_to_yield_.end() ? iter->second : -1;
  }

  int getLastYieldId()
  {
    return next_yield_id_ - 1;
  }

  int getPriorYieldId(int yield_id)
  {
    auto iter = yield_to_prior_yield_.find(yield_id);
    return iter != yield_to_prior_yield_.end() ? iter->second : 0;
  }

  bool isReachable(int from_yield_id, int to_yield_id)
  {
    return is_reachable_yield_.count(std::make_pair(from_yield_id, to_yield_id)) > 0;
  }

  bool TraverseCompoundStmt(CompoundStmt* stmt)
  {
    int curr_scope_id = next_scope_id_;
    curr_scope_path_.push_back(curr_scope_id);
    next_scope_id_ = 0;
    int enclosing_scope_yield_id = curr_scope_yield_id_;
    ptr_to_yield_[stmt] = enclosing_scope_yield_id;

    for (CompoundStmt::body_iterator b = stmt->body_begin(), e = stmt->body_end(); b != e; ++b)
      TraverseStmt(*b);

    curr_scope_yield_id_ = enclosing_scope_yield_id;
    curr_scope_path_.pop_back();
    next_scope_id_ = curr_scope_id + 1;

    return true;
  }

  bool TraverseForStmt(ForStmt* stmt)
  {
    int curr_scope_id = next_scope_id_;
    curr_scope_path_.push_back(curr_scope_id);
    next_scope_id_ = 0;
    int enclosing_scope_yield_id = curr_scope_yield_id_;
    ptr_to_yield_[stmt] = enclosing_scope_yield_id;

    if (stmt->getInit())
      TraverseStmt(stmt->getInit());

    curr_scope_path_.push_back(next_scope_id_);
    next_scope_id_ = 0;

    TraverseStmt(stmt->getBody());

    curr_scope_yield_id_ = enclosing_scope_yield_id;
    curr_scope_path_.pop_back();
    curr_scope_path_.pop_back();
    next_scope_id_ = curr_scope_id + 1;

    return true;
  }

  bool TraverseWhileStmt(WhileStmt* stmt)
  {
    int curr_scope_id = next_scope_id_;
    curr_scope_path_.push_back(curr_scope_id);
    next_scope_id_ = 0;
    int enclosing_scope_yield_id = curr_scope_yield_id_;

    if (stmt->getCond())
      TraverseStmt(stmt->getCond());

    curr_scope_path_.push_back(next_scope_id_);
    next_scope_id_ = 0;

    TraverseStmt(stmt->getBody());

    curr_scope_yield_id_ = enclosing_scope_yield_id;
    curr_scope_path_.pop_back();
    curr_scope_path_.pop_back();
    next_scope_id_ = curr_scope_id + 1;

    return true;
  }

  bool TraverseIfStmt(IfStmt* stmt)
  {
    int curr_scope_id = next_scope_id_;
    curr_scope_path_.push_back(curr_scope_id);
    next_scope_id_ = 0;
    int enclosing_scope_yield_id = curr_scope_yield_id_;

    if (stmt->getCond())
      TraverseStmt(stmt->getCond());

    curr_scope_path_.push_back(0);
    next_scope_id_ = 0;
    int enclosing_scope_yield_id_2 = curr_scope_yield_id_;

    TraverseStmt(stmt->getThen());

    ++curr_scope_path_.back();
    next_scope_id_ = 0;
    curr_scope_yield_id_ = enclosing_scope_yield_id_2;

    TraverseStmt(stmt->getElse());

    curr_scope_yield_id_ = enclosing_scope_yield_id;
    curr_scope_path_.pop_back();
    curr_scope_path_.pop_back();
    next_scope_id_ = curr_scope_id + 1;

    return true;
  }

  bool VisitVarDecl(VarDecl* decl)
  {
    if (decl->hasLocalStorage())
    {
      scope_to_decl_.insert(std::make_pair(curr_scope_path_, decl));
      decl_to_scope_.insert(std::make_pair(decl, curr_scope_path_));

      if (decl->hasInit())
      {
        if (CXXConstructExpr::classof(decl->getInit()) || ExprWithCleanups::classof(decl->getInit()))
          AddYieldPoint(decl);
      }
    }

    return true;
  }

  bool VisitConditionalOperator(ConditionalOperator* op)
  {
    if (IsYieldKeyword(op))
      AddYieldPoint(op);

    return true;
  }

  bool VisitReturnStmt(ReturnStmt* stmt)
  {
    if (IsFromKeyword(stmt->getRetValue()))
      AddYieldPoint(stmt);

    return true;
  }

private:
  void AddYieldPoint(void* ptr)
  {
    int yield_id = next_yield_id_++;
    int prior_yield_id = curr_scope_yield_id_;
    ptr_to_yield_[ptr] = yield_id;
    yield_to_prior_yield_[yield_id] = prior_yield_id;
    curr_scope_yield_id_ = yield_id;
    while (prior_yield_id > 0)
    {
      is_reachable_yield_.insert(std::make_pair(prior_yield_id, yield_id));
      prior_yield_id = getPriorYieldId(prior_yield_id);
    }
  }

  scope_path curr_scope_path_;
  int next_scope_id_;
  int next_yield_id_;
  int curr_scope_yield_id_;
  std::multimap<scope_path, ValueDecl*> scope_to_decl_;
  std::unordered_map<ValueDecl*, scope_path> decl_to_scope_;
  std::unordered_map<void*, int> ptr_to_yield_;
  std::unordered_map<int, int> yield_to_prior_yield_;
  std::set<std::pair<int, int>> is_reachable_yield_;
};

//------------------------------------------------------------------------------
// This class is responsible for the main job of generating the code associated
// with a resumable lambda.

class resumable_lambda_codegen :
  public RecursiveASTVisitor<resumable_lambda_codegen>
{
public:
  resumable_lambda_codegen(Rewriter& r, LambdaExpr* expr)
    : rewriter_(r),
      lambda_expr_(expr),
      lambda_id_(next_lambda_id_++)
  {
  }

  void Generate()
  {
    if (!resumable_lambda_detector().IsResumable(lambda_expr_))
      return;

    locals_.Build(lambda_expr_);
    CompoundStmt* body = lambda_expr_->getBody();
    SourceRange beforeBody(lambda_expr_->getLocStart(), body->getLocStart());
    SourceRange afterBody(body->getLocEnd(), lambda_expr_->getLocEnd());

    std::stringstream before;
    before << "/*BEGIN RESUMABLE LAMBDA DEFINITION*/\n\n";
    before << "[&]{\n";
    EmitCaptureTypedefs(before);
    before << "  struct __resumable_lambda_" << lambda_id_ << "_capture\n";
    before << "  {\n";
    EmitCaptureConstructor(before);
    before << "\n";
    EmitCaptureMembers(before);
    before << "  };\n";
    before << "\n";
    before << "  struct __resumable_lambda_" << lambda_id_ << "_locals_data\n";
    before << "  {\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_locals_data() {}\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_locals_data(const __resumable_lambda_" << lambda_id_ << "_locals_data&) = delete;\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_locals_data(__resumable_lambda_" << lambda_id_ << "_locals_data&&) = delete;\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_locals_data& operator=(__resumable_lambda_" << lambda_id_ << "_locals_data&&) = delete;\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_locals_data& operator=(const __resumable_lambda_" << lambda_id_ << "_locals_data&) = delete;\n";
    before << "    ~__resumable_lambda_" << lambda_id_ << "_locals_data() {}\n";
    before << "\n";
    EmitLocalsDataMembers(before);
    before << "\n";
    EmitLocalsDataUnwindTo(before);
    before << "  };\n";
    before << "\n";
    before << "  struct __resumable_lambda_" << lambda_id_ << "_locals_unwinder\n";
    before << "  {\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_locals_data* __locals;\n";
    before << "\n";
    before << "    ~__resumable_lambda_" << lambda_id_ << "_locals_unwinder()\n";
    before << "    {\n";
    before << "      if (__locals)\n";
    before << "        __locals->__unwind_to(-1);\n";
    before << "    }\n";
    before << "  };\n";
    before << "\n";
    before << "  struct __resumable_lambda_" << lambda_id_ << "_locals :\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_locals_data\n";
    before << "  {\n";
    EmitLocalsConstructor(before);
    before << "\n";
    EmitLocalsCopyConstructor(before);
    before << "\n";
    EmitLocalsMoveConstructor(before);
    before << "\n";
    EmitLocalsDestructor(before);
    before << "  };\n";
    before << "\n";
    before << "  struct __resumable_lambda_" << lambda_id_ << " :\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_capture,\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_locals\n";
    before << "  {\n";
    EmitConstructor(before);
    before << "\n";
    before << "    bool is_initial() const noexcept { return __state == 0; }\n";
    before << "    bool is_terminal() const noexcept { return __state == -1; }\n";
    before << "\n";
    EmitCallOperatorDecl(before);
    before << "    {\n";
    before << "      __resumable_lambda_" << lambda_id_ << "_locals_unwinder __unwind = { this };\n";
    before << "      switch (__state)\n";
    before << "      case 0:\n";
    before << "      {\n";
    EmitLineNumber(before, body->getLocStart());
    rewriter_.ReplaceText(beforeBody, before.str());

    TraverseCompoundStmt(body);

    std::stringstream after;
    after << "\n";
    after << "      __unwind_to(-1);\n";
    after << "      default: (void)0;\n";
    after << "      }\n";
    after << "    }\n";
    after << "  };\n";
    EmitReturn(after);
    after << "}()\n\n";
    EmitLineNumber(after, body->getLocEnd());
    after << "/*END RESUMABLE LAMBDA DEFINITION*/";
    rewriter_.ReplaceText(afterBody, after.str());
  }

  bool TraverseCompoundStmt(CompoundStmt* stmt)
  {
    if (stmt != lambda_expr_->getBody())
    {
      rewriter_.InsertTextBefore(stmt->getLocStart(), "{");
    }

    bool result = RecursiveASTVisitor<resumable_lambda_codegen>::TraverseCompoundStmt(stmt);

    if (stmt != lambda_expr_->getBody())
    {
      std::string unwind = "__unwind_to(" + std::to_string(locals_.getYieldId(stmt)) + ");}";
      auto end = Lexer::getLocForEndOfToken(stmt->getLocEnd(), 0, rewriter_.getSourceMgr(), rewriter_.getLangOpts());
      rewriter_.InsertTextAfter(end, unwind);
    }

    return result;
  }

  bool TraverseForStmt(ForStmt* stmt)
  {
    if (stmt->getInit())
    {
      rewriter_.InsertTextBefore(stmt->getLocStart(), "{");
    }

    bool result = RecursiveASTVisitor<resumable_lambda_codegen>::TraverseForStmt(stmt);

    if (stmt->getInit())
    {
      std::string unwind = "__unwind_to(" + std::to_string(locals_.getYieldId(stmt)) + ");}";
      auto end = Lexer::getLocForEndOfToken(stmt->getBody()->getLocEnd(), 0, rewriter_.getSourceMgr(), rewriter_.getLangOpts());
      rewriter_.InsertTextAfterToken(end, unwind);
    }

    return result;
  }

  bool VisitCXXThisExpr(CXXThisExpr* expr)
  {
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      if (c->getCaptureKind() == LCK_This)
      {
        if (expr->isImplicit())
        {
          rewriter_.InsertTextBefore(expr->getLocStart(), "__this->");
        }
        else
        {
          SourceRange range(expr->getLocStart(), expr->getLocEnd());
          rewriter_.ReplaceText(range, "__this");
        }
      }
    }

    return true;
  }

  bool VisitConditionalOperator(ConditionalOperator* op)
  {
    if (Expr* after_yield = IsYieldKeyword(op))
    {
      if (Expr* after_from = IsFromKeyword(after_yield))
      {
        // "yield from"

        auto yield = rewriter_.getSourceMgr().getImmediateExpansionRange(op->getLocStart());
        auto from = rewriter_.getSourceMgr().getImmediateExpansionRange(after_yield->getLocStart());
        auto end = Lexer::findLocationAfterToken(op->getLocEnd(), tok::semi, rewriter_.getSourceMgr(), rewriter_.getLangOpts(), true);
        SourceRange range(yield.first, from.second);

        int yield_point = locals_.getYieldId(op);

        std::stringstream before;
        before << "\n";
        before << "        for (;;)\n";
        before << "        {\n";
        before << "          {\n";
        before << "            auto& __g =\n";
        EmitLineNumber(before, after_from->getLocStart());
        rewriter_.ReplaceText(range, before.str());

        std::stringstream after;
        after << "            if (__g.is_terminal()) break;\n";
        after << "            __state = " << yield_point << ";\n";
        after << "            __unwind.__locals = nullptr;\n";
        after << "            return __g();\n";
        after << "          }\n";
        after << "        case " << yield_point << ":\n";
        after << "          (void)0;\n";
        after << "        }\n";
        rewriter_.InsertTextAfter(end, after.str());
      }
      else
      {
        // "yield"

        auto yield = rewriter_.getSourceMgr().getImmediateExpansionRange(op->getLocStart());
        auto end = Lexer::getLocForEndOfToken(op->getLocEnd(), 0, rewriter_.getSourceMgr(), rewriter_.getLangOpts());
        SourceRange range(yield.first, yield.second);

        int yield_point = locals_.getYieldId(op);

        std::stringstream before;
        before << "\n";
        before << "        do\n";
        before << "        {\n";
        before << "          __state = " << yield_point << ";\n";
        before << "          __unwind.__locals = nullptr;\n";
        before << "          return\n";
        EmitLineNumber(before, after_yield->getLocStart());
        rewriter_.ReplaceText(range, before.str());

        std::stringstream after;
        after << ";\n";
        after << "        case " << yield_point << ":\n";
        after << "          (void)0;\n";
        after << "        } while (false)";
        rewriter_.InsertTextBefore(end, after.str());
      }
    }

    return true;
  }

  bool VisitReturnStmt(ReturnStmt* stmt)
  {
    if (Expr* after_from = IsFromKeyword(stmt->getRetValue()))
    {
      // "return from"

      auto from = rewriter_.getSourceMgr().getImmediateExpansionRange(stmt->getRetValue()->getLocStart());
      auto end = Lexer::findLocationAfterToken(stmt->getLocEnd(), tok::semi, rewriter_.getSourceMgr(), rewriter_.getLangOpts(), true);
      SourceRange range(stmt->getLocStart(), from.second);

      int yield_point = locals_.getYieldId(stmt);

      std::stringstream before;
      before << "\n";
      before << "        for (;;)\n";
      before << "        {\n";
      before << "          {\n";
      before << "            auto& __g =\n";
      EmitLineNumber(before, after_from->getLocStart());
      rewriter_.ReplaceText(range, before.str());

      std::stringstream after;
      after << "            __state = " << yield_point << ";\n";
      after << "            __unwind.__locals = nullptr;\n";
      after << "            auto __ret(__g());\n";
      after << "            if (__g.is_terminal())\n";
      after << "              __state = -1;\n";
      after << "            return __ret;\n";
      after << "          }\n";
      after << "        case " << yield_point << ":\n";
      after << "          (void)0;\n";
      after << "        }\n";
      rewriter_.InsertTextAfter(end, after.str());
    }

    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr* expr)
  {
    std::string var = locals_.getPathAsString(expr->getDecl());
    if (!var.empty())
    {
      SourceRange range(expr->getLocStart(), expr->getLocation());
      rewriter_.ReplaceText(range, var);
    }
    else if (expr->getDecl()->getType().getAsString() == "const struct __lambda_this_t")
    {
      auto yield = rewriter_.getSourceMgr().getImmediateExpansionRange(expr->getLocStart());
      SourceRange range(yield.first, yield.second);
      rewriter_.ReplaceText(range, "this");
    }

    return true;
  }

  bool VisitVarDecl(VarDecl* decl)
  {
    if (decl->hasLocalStorage())
    {
      std::string name = locals_.getPathAsString(decl);

      if (decl->hasInit())
      {
        if (CXXConstructExpr* construct_expr = dyn_cast<CXXConstructExpr>(decl->getInit()))
        {
          std::string init = "new (static_cast<void*>(&" + name + ")) " + decl->getType().getAsString();
          int yield_point = locals_.getYieldId(decl);
          std::string state_change = ", __state = " + std::to_string(yield_point);
          SourceRange range(decl->getLocStart(), construct_expr->getLocStart());
          rewriter_.ReplaceText(range, init);
          rewriter_.InsertTextAfterToken(decl->getLocEnd(), state_change);
        }
        else if (ExprWithCleanups* expr = dyn_cast<ExprWithCleanups>(decl->getInit()))
        {
          if (CXXConstructExpr* construct_expr = dyn_cast<CXXConstructExpr>(expr->getSubExpr()))
          {
            if (MaterializeTemporaryExpr* temp = dyn_cast<MaterializeTemporaryExpr>(construct_expr->getArg(0)))
            {
              std::string init = "new (static_cast<void*>(&" + name + ")) " + decl->getType().getAsString() + "(";
              int yield_point = locals_.getYieldId(decl);
              std::string state_change = "), __state = " + std::to_string(yield_point);
              SourceRange range(decl->getLocStart(), temp->getLocStart().getLocWithOffset(-1));
              rewriter_.ReplaceText(range, init);
              rewriter_.InsertTextAfterToken(decl->getLocEnd(), state_change);
            }
          }
        }
        else
        {
          SourceRange range(decl->getLocStart(), decl->getLocation());
          rewriter_.ReplaceText(range, name);
        }
      }
      else
      {
        SourceRange range(decl->getLocStart(), decl->getLocEnd());
        rewriter_.ReplaceText(range, "(void)0");
      }
    }

    return true;
  }

private:
  void EmitCaptureTypedefs(std::ostream& os)
  {
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      if (c->getCaptureKind() == LCK_This)
      {
        os << "  typedef decltype(this) __resumable_lambda_" << lambda_id_ << "_this_type;\n\n";
        return;
      }
      else if (c->isInitCapture())
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        std::string init = rewriter_.ConvertToString(c->getCapturedVar()->getInit());
        os << "  typedef decltype(" << init << ")";
        os << " __resumable_lambda_" << lambda_id_ << "_" << name << "_type;\n";
      }
      else
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "  typedef decltype(" << name << ")";
        os << " __resumable_lambda_" << lambda_id_ << "_" << name << "_type;\n";
      }
    }

    if (lambda_expr_->capture_begin() != lambda_expr_->capture_end())
      os << "\n";
  }

  void EmitCaptureConstructor(std::ostream& os)
  {
    os << "    explicit __resumable_lambda_" << lambda_id_ << "_capture(int /*dummy*/";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << ",\n";
      if (c->getCaptureKind() == LCK_This)
        os << "        __resumable_lambda_" << lambda_id_ << "_this_type __capture_arg_this";
      else if (c->isInitCapture())
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "        __resumable_lambda_" << lambda_id_ << "_" << name << "_type&& __capture_arg_" << name;
      }
      else if (c->getCaptureKind() == LCK_ByRef)
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "        __resumable_lambda_" << lambda_id_ << "_" << name << "_type& __capture_arg_" << name;
      }
      else
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "        const __resumable_lambda_" << lambda_id_ << "_" << name << "_type& __capture_arg_" << name;
      }
    }
    os << ")";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << (c == lambda_expr_->capture_begin() ? " :\n" : ",\n");
      if (c->getCaptureKind() == LCK_This)
      {
        os << "      __this(__capture_arg_this)";
      }
      else if (c->isInitCapture())
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "      " << name << "(static_cast<__resumable_lambda_" << lambda_id_ << "_" << name << "_type&&>(__capture_arg_" << name << "))";
      }
      else
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "      " << name << "(__capture_arg_" << name << ")";
      }
    }
    os << "\n";
    os << "    {\n";
    os << "    }\n";
  }

  void EmitCaptureMembers(std::ostream& os)
  {
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      if (c->getCaptureKind() == LCK_This)
      {
        os << "    __resumable_lambda_" << lambda_id_ << "_this_type __this;\n";
      }
      else
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "    __resumable_lambda_" << lambda_id_ << "_" << name << "_type";
        if (c->getCaptureKind() == LCK_ByRef)
          os << "&";
        os << " " << name << ";\n";
      }
    }
  }

  void EmitLocalsDataMembers(std::ostream& os)
  {
    resumable_lambda_locals::scope_path curr_path;
    std::string indent = "      ";

    os << "    int __state;\n";
    os << "    union\n";
    os << "    {\n";
    for (resumable_lambda_locals::iterator v = locals_.begin(), e = locals_.end(); v != e; ++v)
    {
      while (curr_path.size() > v->first.size())
      {
        indent.pop_back(), indent.pop_back();
        os << indent << "} __s" << curr_path.back() << ";\n";
        curr_path.pop_back();
      }
      while (curr_path.size() > 0 && curr_path.back() != v->first[curr_path.size() - 1])
      {
        indent.pop_back(), indent.pop_back();
        os << indent << "} __s" << curr_path.back() << ";\n";
        curr_path.pop_back();
      }
      for (bool first = true; curr_path.size() < v->first.size(); first = false)
      {
        os << indent << "struct\n";
        os << indent << "{\n";
        indent += "  ";
        curr_path.push_back(v->first[curr_path.size()]);
      }
      std::string type = v->second->getType().getAsString();
      std::string name = v->second->getDeclName().getAsString();
      os << indent << type << " " << name << ";\n";
    }
    while (curr_path.size() > 0)
    {
      indent.pop_back(), indent.pop_back();
      os << indent << "} __s" << curr_path.back() << ";\n";
      curr_path.pop_back();
    }
    os << "    };\n";
  }

  void EmitLocalsDataUnwindTo(std::ostream& os)
  {
    int yield_id = locals_.getLastYieldId();

    os << "    void __unwind_to(int __new_state)\n";
    os << "    {\n";
    os << "      while (__state > __new_state)\n";
    os << "      {\n";
    os << "        switch (__state)\n";
    os << "        {\n";
    for (resumable_lambda_locals::reverse_iterator v = locals_.rbegin(), e = locals_.rend(); v != e; ++v)
    {
      int current_yield_id = locals_.getYieldId(v->second);
      if (current_yield_id > 0)
      {
        for (; yield_id >= current_yield_id; --yield_id)
          os << "        case " << yield_id << ":\n";

        std::string type = v->second->getType().getAsString();
        std::string name = locals_.getPathAsString(v->second);
        os << "          {\n";
        os << "            typedef " << type << " __type;\n";
        os << "            " << name << ".~__type();\n";
        os << "            __state = " << locals_.getPriorYieldId(current_yield_id) << ";\n";
        os << "            break;\n";
        os << "          }\n";
      }
    }
    os << "        case 0: default:\n";
    os << "          __state = -1;\n";
    os << "          break;\n";
    os << "        }\n";
    os << "      }\n";
    os << "    }\n";
  }

  void EmitLocalsConstructor(std::ostream& os)
  {
    os << "    __resumable_lambda_" << lambda_id_ << "_locals()\n";
    os << "    {\n";
    os << "      __state = 0;\n";
    os << "    }\n";
  }

  void EmitLocalsCopyConstructor(std::ostream& os)
  {
    os << "    enum { __is_copy_constructible_v =\n";
    for (resumable_lambda_locals::iterator v = locals_.begin(), e = locals_.end(); v != e; ++v)
      os << "      ::std::is_copy_constructible<" << v->second->getType().getAsString() << ">::value &&\n";
    os << "      true };\n";
    os << "\n";
    os << "    typedef ::std::integral_constant<bool, __is_copy_constructible_v> __is_copy_constructible;\n";
    os << "\n";
    os << "    typedef typename ::std::conditional<__is_copy_constructible_v,\n";
    os << "        __resumable_lambda_" << lambda_id_ << "_locals,\n";
    os << "        __resumable_copy_disabled<__resumable_lambda_" << lambda_id_ << "_locals_data>\n";
    os << "      >::type __copy_constructor_arg;\n";
    os << "\n";
    os << "    __resumable_lambda_" << lambda_id_ << "_locals(const __copy_constructor_arg& __other)\n";
    os << "    {\n";
    os << "      __resumable_lambda_" << lambda_id_ << "_locals_unwinder __unwind = { this };\n";
    for (resumable_lambda_locals::iterator v = locals_.begin(), e = locals_.end(); v != e; ++v)
    {
      std::string type = v->second->getType().getAsString();
      std::string name = locals_.getPathAsString(v->second);
      int yield_id = locals_.getYieldId(v->second);

      os << "      switch (__other.__state)\n";
      os << "      {\n";
      for (int i = 0; i <= locals_.getLastYieldId(); ++i)
        if (yield_id == i || locals_.isReachable(yield_id, i))
          os << "      case " << i << ":\n";
      os << "        __resumable_local_new(__is_copy_constructible(), &" + name + ", __other." + name + ");\n";
      os << "        __state = " << locals_.getYieldId(v->second) << ";\n";
      os << "        break;\n";
      os << "      default:\n";
      os << "        break;\n";
      os << "      }\n";
    }
    os << "      __state = __other.__state;\n";
    os << "      __unwind.__locals = nullptr;\n";
    os << "    }\n";
  }

  void EmitLocalsMoveConstructor(std::ostream& os)
  {
    os << "    enum { __is_move_constructible_v =\n";
    for (resumable_lambda_locals::iterator v = locals_.begin(), e = locals_.end(); v != e; ++v)
      os << "      ::std::is_move_constructible<" << v->second->getType().getAsString() << ">::value &&\n";
    os << "      true };\n";
    os << "\n";
    os << "    typedef ::std::integral_constant<bool, __is_move_constructible_v> __is_move_constructible;\n";
    os << "\n";
    os << "    typedef typename ::std::conditional<__is_move_constructible_v,\n";
    os << "        __resumable_lambda_" << lambda_id_ << "_locals,\n";
    os << "        __resumable_move_disabled<__resumable_lambda_" << lambda_id_ << "_locals_data>\n";
    os << "      >::type __move_constructor_arg;\n";
    os << "\n";
    os << "    __resumable_lambda_" << lambda_id_ << "_locals(__move_constructor_arg&& __other)\n";
    os << "    {\n";
    os << "      __resumable_lambda_" << lambda_id_ << "_locals_unwinder __unwind = { this };\n";
    os << "      __resumable_lambda_" << lambda_id_ << "_locals_unwinder __unwind_other = { &__other };\n";
    for (resumable_lambda_locals::iterator v = locals_.begin(), e = locals_.end(); v != e; ++v)
    {
      std::string type = v->second->getType().getAsString();
      std::string name = locals_.getPathAsString(v->second);
      int yield_id = locals_.getYieldId(v->second);

      os << "      switch (__other.__state)\n";
      os << "      {\n";
      for (int i = 0; i <= locals_.getLastYieldId(); ++i)
        if (yield_id == i || locals_.isReachable(yield_id, i))
          os << "      case " << i << ":\n";
      os << "        __resumable_local_new(__is_move_constructible(), &" + name + ", static_cast<" + type + "&&>(__other." + name + "));\n";
      os << "        __state = " << locals_.getYieldId(v->second) << ";\n";
      os << "        break;\n";
      os << "      default:\n";
      os << "        break;\n";
      os << "      }\n";
    }
    os << "      __state = __other.__state;\n";
    os << "      __unwind.__locals = nullptr;\n";
    os << "    }\n";
  }

  void EmitLocalsDestructor(std::ostream& os)
  {
    os << "    ~__resumable_lambda_" << lambda_id_ << "_locals()\n";
    os << "    {\n";
    os << "      __unwind_to(-1);\n";
    os << "    }\n";
  }

  void EmitConstructor(std::ostream& os)
  {
    os << "    explicit __resumable_lambda_" << lambda_id_ << "(int /*dummy*/";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << ",\n";
      if (c->getCaptureKind() == LCK_This)
      {
        os << "        __resumable_lambda_" << lambda_id_ << "_this_type __capture_arg_this";
      }
      else if (c->isInitCapture())
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        std::string init = rewriter_.ConvertToString(c->getCapturedVar()->getInit());
        os << "        __resumable_lambda_" << lambda_id_ << "_" << name << "_type&& __capture_arg_" << name;
      }
      else if (c->getCaptureKind() == LCK_ByRef)
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "        __resumable_lambda_" << lambda_id_ << "_" << name << "_type& __capture_arg_" << name;
      }
      else
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "        const __resumable_lambda_" << lambda_id_ << "_" << name << "_type& __capture_arg_" << name;
      }
    }
    os << ") :\n";
    os << "      __resumable_lambda_" << lambda_id_ << "_capture(0 /*dummy*/";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << ",\n";
      if (c->getCaptureKind() == LCK_This)
        os << "          __capture_arg_this";
      else if (c->isInitCapture())
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "          static_cast<__resumable_lambda_" << lambda_id_ << "_" << name << "_type&&>(__capture_arg_" << name << ")";
      }
      else
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "          __capture_arg_" << name;
      }
    }
    os << ")\n";
    os << "    {\n";
    os << "    }\n";
  }

  void EmitCallOperatorDecl(std::ostream& os)
  {
    CXXMethodDecl* method = lambda_expr_->getCallOperator();
    os << "    ";
    if (lambda_expr_->hasExplicitResultType())
      os << method->getReturnType().getAsString();
    else
      os << "auto";
    os << " operator()(";
    for (FunctionDecl::param_iterator p = method->param_begin(), e = method->param_end(); p != e; ++p)
    {
      if (p != method->param_begin())
        os << ",";
      os << "\n      " << (*p)->getType().getAsString() << " " << (*p)->getNameAsString();
    }
    os << ")\n";
  }

  void EmitReturn(std::ostream& os)
  {
    os << "  return __resumable_lambda_" << lambda_id_ << "(0 /*dummy*/";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << ",\n";
      if (c->getCaptureKind() == LCK_This)
        os << "    this";
      else if (c->isInitCapture())
        os << "    " << rewriter_.ConvertToString(c->getCapturedVar()->getInit());
      else
        os << "    " << c->getCapturedVar()->getDeclName().getAsString();
    }
    os << "\n";
    os << "  );\n";
  }

  void EmitLineNumber(std::ostream& os, SourceLocation location)
  {
    StringRef file_name = rewriter_.getSourceMgr().getFilename(location);
    if (file_name.data())
    {
      os << "#line ";
      os << rewriter_.getSourceMgr().getExpansionLineNumber(location);
      os << " \"" << file_name.data() << "\"";
      os << "\n";
    }
  }

  Rewriter& rewriter_;
  LambdaExpr* lambda_expr_;
  int lambda_id_;
  static int next_lambda_id_;
  resumable_lambda_locals locals_;
};

int resumable_lambda_codegen::next_lambda_id_ = 0;

//------------------------------------------------------------------------------
// This class visits the AST looking for potential resumable lambdas.

class main_visitor : public RecursiveASTVisitor<main_visitor>
{
public:
  main_visitor(Rewriter& r)
    : rewriter_(r)
  {
  }

  bool VisitLambdaExpr(LambdaExpr* expr)
  {
    resumable_lambda_codegen(rewriter_, expr).Generate();
    return true;
  }

private:
  Rewriter& rewriter_;
};

//------------------------------------------------------------------------------
// This class is used by the compiler to process all top level declarations.

class consumer : public ASTConsumer
{
public:
  consumer(Rewriter& r)
    : visitor_(r)
  {
  }

  bool HandleTopLevelDecl(DeclGroupRef decls) override
  {
    for (DeclGroupRef::iterator b = decls.begin(), e = decls.end(); b != e; ++b)
    {
      (*b)->dump();
      visitor_.TraverseDecl(*b);
    }

    return true;
  }

private:
  main_visitor visitor_;
};

//------------------------------------------------------------------------------
// This class handles notifications from the compiler frontend.

class frontend_action : public ASTFrontendAction
{
public:
  bool BeginSourceFileAction(CompilerInstance& compiler, StringRef file_name) override
  {
    compiler.getPreprocessor().addPPCallbacks(new code_injector(compiler.getPreprocessor()));
    return true;
  }

  void EndSourceFileAction() override
  {
    SourceManager& mgr = rewriter_.getSourceMgr();
    llvm::errs() << "** EndSourceFileAction for: "
                 << mgr.getFileEntryForID(mgr.getMainFileID())->getName() << "\n";
    rewriter_.getEditBuffer(mgr.getMainFileID()).write(llvm::outs());
  }

  ASTConsumer* CreateASTConsumer(CompilerInstance& compiler, StringRef file) override
  {
    llvm::errs() << "** Creating AST consumer for: " << file << "\n";
    rewriter_.setSourceMgr(compiler.getSourceManager(), compiler.getLangOpts());
    std::string preamble = "#ifndef __RESUMABLE_PREAMBLE\n";
    preamble += "#define __RESUMABLE_PREAMBLE\n";
    preamble += "\n";
    preamble += "#include <new>\n";
    preamble += "#include <type_traits>\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "struct __resumable_copy_disabled : _T {};\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "struct __resumable_move_disabled : _T {};\n";
    preamble += "\n";
    preamble += "template <class _T, class... _Args>\n";
    preamble += "inline void __resumable_local_new(::std::true_type, _T* __p, _Args&&... __args)\n";
    preamble += "{\n";
    preamble += "  new (static_cast<void*>(__p)) _T(static_cast<_Args&&>(__args)...);\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T, class... _Args>\n";
    preamble += "inline void __resumable_local_new(::std::false_type, _T*, _Args&&...)\n";
    preamble += "{\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "#endif // __RESUMABLE_PREAMBLE\n";
    preamble += "\n";
    preamble += std::string("#line 1 \"") + file.data() + "\"\n";
    rewriter_.InsertText(rewriter_.getSourceMgr().getLocForStartOfFile(rewriter_.getSourceMgr().getMainFileID()), preamble);
    return new consumer(rewriter_);
  }

private:
  Rewriter rewriter_;
};

//------------------------------------------------------------------------------

int main(int argc, const char* argv[])
{
  if (argc < 2)
  {
    std::cerr << "Usage: resumable-pp <source> [clang args]\n";
    return 1;
  }

  std::vector<std::string> args;
  args.push_back("-std=c++1y");
  for (int arg = 2; arg < argc; ++arg)
    args.push_back(argv[arg]);

  std::vector<std::string> files;
  files.push_back(argv[1]);

  FixedCompilationDatabase cdb(".", args);
  ClangTool tool(cdb, files);
  return tool.run(newFrontendActionFactory<frontend_action>().get());
}
