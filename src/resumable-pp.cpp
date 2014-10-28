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

std::string allowed_path;
bool verbose = false;

//------------------------------------------------------------------------------
// The following code is injected at the beginning of the preprocessor input.

const char injected[] = R"-(

#include <typeinfo>

struct __yield_t
{
  constexpr __yield_t() {}
  template <class T> operator T() const;
  template <class T> __yield_t operator&(const T&) const { return {}; }
};

constexpr __yield_t __yield;

struct __from_t
{
  constexpr __from_t() {}
  template <class T> operator T() const;
  template <class T> __from_t operator&(const T&) const { return {}; }
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

template <class _T> bool is_initial(const _T&) noexcept { return false; }
template <class _T> bool is_terminal(const _T&) noexcept { return false; }
template <class _T> const std::type_info& wanted_type(const _T&) noexcept { return typeid(void); }
template <class _T> void* wanted(_T&) noexcept { return nullptr; }
template <class _T> const void* wanted(const _T&) noexcept { return nullptr; }
template <class _T> _T initializer(_T&& __t) { return static_cast<_T&&>(__t); }
template <class _T> struct lambda { typedef _T type; };
template <class _T> using lambda_t = typename lambda<_T>::type;

#define resumable __attribute__((__annotate__("resumable"))) mutable
#define yield 0 ? throw __yield : throw
#define from __from&
#define lambda_this __lambda_this

)-";

//------------------------------------------------------------------------------
// Helper function to check a filename.

void check_filename(const std::string& filename)
{
  if (!allowed_path.empty())
  {
    if (filename.find(allowed_path) != 0)
      exit(1);
    if (filename.find("..") != std::string::npos)
      exit(1);
    if (filename.find_first_of("\"$&'()*;<>?[\\]`{|}~ \t\r\n") != std::string::npos)
      exit(1);
    for (std::size_t i = 0; i < filename.length(); ++i)
      if (!isprint(filename[i]))
        exit(1);
  }
}

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
    else
    {
      check_filename(source_mgr.getFilename(loc));
      check_filename(source_mgr.getFilename(source_mgr.getExpansionLoc(loc)));
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
  if (ExprWithCleanups* expr = dyn_cast<ExprWithCleanups>(stmt))
    stmt = expr->getSubExpr();
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
    if (attr && attr->getAnnotation() == "resumable")
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
// Replaces the text of each local variable in situ to reflect the new name.

class resumable_lambda_locals:
  public RecursiveASTVisitor<resumable_lambda_locals>
{
public:
  struct local
  {
    std::string type;
    std::string name;
    std::string full_name;
    std::string generator_expr;
    int yield_id;
  };

  typedef std::vector<int> scope_path;
  typedef std::map<scope_path, local>::iterator iterator;

  explicit resumable_lambda_locals(Rewriter& rewriter)
    : rewriter_(rewriter)
  {
  }

  void Build(LambdaExpr* expr)
  {
    curr_scope_path_.clear();
    next_scope_id_ = 0;
    curr_yield_id_ = 1;
    curr_scope_yield_id_ = 1;
    TraverseCompoundStmt(expr->getBody());
  }

  iterator begin()
  {
    return scope_to_local_.begin();
  }

  iterator end()
  {
    return scope_to_local_.end();
  }

  iterator find(ValueDecl* decl)
  {
    auto iter = ptr_to_iter_.find(decl);
    return iter != ptr_to_iter_.end() ? iter->second : end();
  }

  iterator find(MaterializeTemporaryExpr* temp)
  {
    auto iter = ptr_to_iter_.find(temp);
    return iter != ptr_to_iter_.end() ? iter->second : end();
  }

  iterator find(int yield_id)
  {
    auto iter = yield_to_iter_.find(yield_id);
    return iter != yield_to_iter_.end() ? iter->second : end();
  }

  int getYieldId(ValueDecl* decl)
  {
    auto iter = ptr_to_yield_.find(decl);
    return iter != ptr_to_yield_.end() ? iter->second : -1;
  }

  int getYieldId(MaterializeTemporaryExpr* temp)
  {
    auto iter = ptr_to_yield_.find(temp);
    return iter != ptr_to_yield_.end() ? iter->second : -1;
  }

  int getYieldId(Stmt* stmt)
  {
    auto iter = ptr_to_yield_.find(stmt);
    return iter != ptr_to_yield_.end() ? iter->second : -1;
  }

  int getLastYieldId()
  {
    return curr_yield_id_;
  }

  int getPriorYieldId(int yield_id)
  {
    auto iter = yield_to_prior_yield_.find(yield_id);
    return iter != yield_to_prior_yield_.end() ? iter->second : 0;
  }

  std::string getSubGenerator(int yield_id)
  {
    auto iter = yield_to_subgen_.find(yield_id);
    return iter != yield_to_subgen_.end() ? iter->second : "";
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

    if (stmt->getCond())
      TraverseStmt(stmt->getCond());

    if (stmt->getInc())
      TraverseStmt(stmt->getInc());

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

  bool TraverseVarDecl(VarDecl* decl)
  {
    bool result = RecursiveASTVisitor<resumable_lambda_locals>::TraverseVarDecl(decl);

    if (decl->hasLocalStorage())
    {
      int yield_id = AddYieldPoint(decl);
      std::string type = decl->getType().getAsString();
      std::string name = decl->getDeclName().getAsString();
      std::string full_name;
      for (int scope: curr_scope_path_)
        full_name += "__s" + std::to_string(scope) + ".";
      full_name += name;
      iterator iter = scope_to_local_.insert(std::make_pair(curr_scope_path_, local{type, name, full_name, "", yield_id}));
      ptr_to_iter_[decl] = iter;

      if (decl->hasInit())
      {
        yield_to_iter_[yield_id] = iter;

        if (CXXConstructExpr* construct_expr = dyn_cast<CXXConstructExpr>(decl->getInit()))
        {
          std::string init = "new (static_cast<void*>(&" + full_name + ")) " + type + "(";
          if (construct_expr->getNumArgs() > 0)
            init += rewriter_.getRewrittenText(construct_expr->getParenOrBraceRange());
          init += "), __state = " + std::to_string(yield_id);
          rewriter_.ReplaceText(SourceRange(decl->getLocStart(), decl->getLocEnd()), init);
        }
        else if (ExprWithCleanups* expr = dyn_cast<ExprWithCleanups>(decl->getInit()))
        {
          if (CXXConstructExpr* construct_expr = dyn_cast<CXXConstructExpr>(expr->getSubExpr()))
          {
            if (MaterializeTemporaryExpr* temp = dyn_cast<MaterializeTemporaryExpr>(construct_expr->getArg(0)))
            {
              std::string init = "new (static_cast<void*>(&" + full_name + ")) " + type + "(";
              init += rewriter_.getRewrittenText(SourceRange(temp->getLocStart(), temp->getLocEnd()));
              init += "), __state = " + std::to_string(yield_id);
              rewriter_.ReplaceText(SourceRange(decl->getLocStart(), decl->getLocEnd()), init);
            }
          }
        }
        else
        {
          SourceRange range(decl->getLocStart(), decl->getLocation());
          rewriter_.ReplaceText(range, full_name);
        }
      }
      else
      {
        SourceRange range(decl->getLocStart(), decl->getLocEnd());
        rewriter_.ReplaceText(range, "__state = " + std::to_string(yield_id));
      }
    }

    return result;
  }

  bool TraverseConditionalOperator(ConditionalOperator* op)
  {
    bool result = RecursiveASTVisitor<resumable_lambda_locals>::TraverseConditionalOperator(op);

    if (Expr* after_yield = IsYieldKeyword(op))
    {
      if (Expr* after_from = IsFromKeyword(after_yield))
      {
        if (MaterializeTemporaryExpr* temp = dyn_cast<MaterializeTemporaryExpr>(after_from))
        {
          AddGenerator(op, temp);
        }
        else
        {
          int yield_id = AddYieldPoint(op);
          yield_to_subgen_[yield_id] = rewriter_.getRewrittenText(SourceRange(after_from->getLocStart(), after_from->getLocEnd().getLocWithOffset(1)));
        }

        return result;
      }

      AddYieldPoint(op);
    }

    return result;
  }

  bool TraverseReturnStmt(ReturnStmt* stmt)
  {
    bool result = RecursiveASTVisitor<resumable_lambda_locals>::TraverseReturnStmt(stmt);

    if (Expr* after_from = IsFromKeyword(stmt->getRetValue()))
    {
      if (MaterializeTemporaryExpr* temp = dyn_cast<MaterializeTemporaryExpr>(after_from))
      {
        AddGenerator(stmt, temp);

        return result;
      }

      int yield_id = AddYieldPoint(stmt);
      yield_to_subgen_[yield_id] = rewriter_.getRewrittenText(SourceRange(after_from->getLocStart(), after_from->getLocEnd().getLocWithOffset(1)));
    }

    return result;
  }

  bool TraverseDeclRefExpr(DeclRefExpr* expr)
  {
    bool result = RecursiveASTVisitor<resumable_lambda_locals>::TraverseDeclRefExpr(expr);

    iterator iter = find(expr->getDecl());
    if (iter != end())
    {
      rewriter_.ReplaceText(SourceRange(expr->getLocStart(), expr->getLocEnd()), iter->second.full_name);
    }
    else if (expr->getDecl()->getType().getAsString() == "const struct __lambda_this_t")
    {
      auto yield = rewriter_.getSourceMgr().getImmediateExpansionRange(expr->getLocStart());
      SourceRange range(yield.first, yield.second);
      rewriter_.ReplaceText(range, "this");
    }

    return result;
  }

private:
  int AddYieldPoint(void* ptr)
  {
    int yield_id = ++curr_yield_id_;
    int prior_yield_id = curr_scope_yield_id_;
    ptr_to_yield_[ptr] = yield_id;
    yield_to_prior_yield_[yield_id] = prior_yield_id;
    curr_scope_yield_id_ = yield_id;
    while (prior_yield_id > 0)
    {
      is_reachable_yield_.insert(std::make_pair(prior_yield_id, yield_id));
      prior_yield_id = getPriorYieldId(prior_yield_id);
    }
    return yield_id;
  }

  void AddGenerator(Stmt* parent, MaterializeTemporaryExpr* temp)
  {
    int curr_scope_id = next_scope_id_;
    curr_scope_path_.push_back(curr_scope_id);
    next_scope_id_ = 0;
    int enclosing_scope_yield_id = curr_scope_yield_id_;

    int temp_yield_id = AddYieldPoint(temp);

    std::string inner_type = rewriter_.ConvertToString(temp);
    std::string type = "__resumable_generator_type<decltype(" + inner_type + ")>::_Type";
    std::string name = "__temp" + std::to_string(temp_yield_id);
    std::string full_name;
    for (int scope: curr_scope_path_)
      full_name += "__s" + std::to_string(scope) + ".";
    full_name += name;

    std::string expr = "__resumable_generator_init(&" + full_name + "), ";
    expr += "__state = " + std::to_string(temp_yield_id) + ", ";
    expr += "__resumable_generator_construct(&" + full_name + ", ";
    expr += rewriter_.getRewrittenText(SourceRange(temp->getLocStart(), temp->getLocEnd())) + ")";

    /*std::string expr = "new (static_cast<void*>(&" + full_name + ")) decltype(" + full_name + ")(";
    expr += rewriter_.getRewrittenText(SourceRange(temp->getLocStart(), temp->getLocEnd()));
    expr += "), __state = " + std::to_string(temp_yield_id);*/

    iterator iter = scope_to_local_.insert(std::make_pair(curr_scope_path_, local{type, name, full_name, expr, temp_yield_id}));
    ptr_to_iter_[temp] = iter;
    yield_to_iter_[temp_yield_id] = iter;

    SourceRange range(temp->getLocStart(), temp->getLocEnd());
    rewriter_.ReplaceText(range, full_name);

    int yield_id = AddYieldPoint(parent);
    yield_to_iter_[yield_id] = iter;

    yield_to_subgen_[yield_id] = full_name;
    yield_to_subgen_[temp_yield_id] = full_name;

    curr_scope_yield_id_ = enclosing_scope_yield_id;
    curr_scope_path_.pop_back();
    next_scope_id_ = curr_scope_id + 1;
  }

  Rewriter& rewriter_;
  scope_path curr_scope_path_;
  int next_scope_id_;
  int curr_yield_id_;
  int curr_scope_yield_id_;
  std::multimap<scope_path, local> scope_to_local_;
  std::unordered_map<void*, iterator> ptr_to_iter_;
  std::unordered_map<void*, int> ptr_to_yield_;
  std::unordered_map<int, iterator> yield_to_iter_;
  std::unordered_map<int, int> yield_to_prior_yield_;
  std::set<std::pair<int, int>> is_reachable_yield_;
  std::unordered_map<int, std::string> yield_to_subgen_;
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
      lambda_id_(next_lambda_id_++),
      locals_(r)
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
    before << "  struct __resumable_lambda_" << lambda_id_ << ";\n";
    before << "  struct __resumable_lambda_" << lambda_id_ << "_in_place;\n";
    before << "\n";
    before << "  struct __resumable_lambda_" << lambda_id_ << "_initializer\n";
    before << "  {\n";
    before << "    typedef __resumable_lambda_" << lambda_id_ << " lambda __RESUMABLE_UNUSED_TYPEDEF;\n";
    before << "    typedef __resumable_lambda_" << lambda_id_ << "_in_place generator_type __RESUMABLE_UNUSED_TYPEDEF;\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_capture __capture;\n";
    before << "  };\n";
    before << "\n";
    before << "  struct __resumable_lambda_" << lambda_id_ << " :\n";
    before << "    private __resumable_lambda_" << lambda_id_ << "_capture,\n";
    before << "    private __resumable_lambda_" << lambda_id_ << "_locals\n";
    before << "  {\n";
    EmitConstructor(before);
    before << "\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_initializer operator*() &&\n";
    before << "    {\n";
    before << "      return { static_cast<__resumable_lambda_" << lambda_id_ << "_capture&&>(*this) };\n";
    before << "    }\n";
    before << "\n";
    before << "    bool is_initial() const noexcept { return __state == 0; }\n";
    before << "    bool is_terminal() const noexcept { return __state == -1; }\n";
    before << "\n";
    EmitWantedType(before);
    before << "\n";
    EmitWanted(before);
    before << "\n";
    EmitCallOperatorDecl(before);
    before << "    {\n";
    before << "      __resumable_lambda_" << lambda_id_ << "_locals_unwinder __unwind = { this };\n";
    before << "      switch (__state)\n";
    before << "      {\n";
    before << "      case 0:\n";
    before << "        __state = 1;\n";
    before << "      case 1:\n";
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
    after << "\n";
    EmitInPlaceGenerator(after);
    after << "\n";
    EmitFactory(after);
    after << "}()";
    EmitCreation(after);
    after << "\n\n";
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

  bool TraverseConditionalOperator(ConditionalOperator* op)
  {
    bool result = RecursiveASTVisitor<resumable_lambda_codegen>::TraverseConditionalOperator(op);

    if (Expr* after_yield = IsYieldKeyword(op))
    {
      if (Expr* after_from = IsFromKeyword(after_yield))
      {
        // "yield from"

        SourceRange range(after_from->getLocStart(), Lexer::getLocForEndOfToken(after_from->getLocEnd(), 0, rewriter_.getSourceMgr(), rewriter_.getLangOpts()));
        std::string expr = rewriter_.getRewrittenText(range);

        int yield_point = locals_.getYieldId(op);

        std::stringstream os;
        os << "\n";
        os << "        do\n";
        os << "        {\n";
        if (MaterializeTemporaryExpr* temp = dyn_cast<MaterializeTemporaryExpr>(after_from))
        {
          resumable_lambda_locals::iterator iter = locals_.find(temp);
          if (iter != locals_.end() && !iter->second.generator_expr.empty())
          {
            EmitLineNumber(os, after_from->getLocStart());
            os << "          " << iter->second.generator_expr << ";\n";
          }
        }
        os << "          __state = " << yield_point << ";\n";
        os << "          for (;;)\n";
        os << "          {\n";
        os << "            {\n";
        EmitLineNumber(os, after_from->getLocStart());
        os << "              auto& __g = " << expr.substr(0, expr.length() - 1) << ";\n";
        os << "              if (__g.is_terminal()) break;\n";
        os << "              __unwind.__locals = nullptr;\n";
        os << "              return __g();\n";
        os << "            }\n";
        if (dyn_cast<MaterializeTemporaryExpr>(after_from))
          os << "          case " << yield_point - 1 << ":\n";
        os << "          case " << yield_point << ":\n";
        os << "            (void)0;\n";
        os << "          }\n";
        if (MaterializeTemporaryExpr* temp = dyn_cast<MaterializeTemporaryExpr>(after_from))
          os << "          __unwind_to(" << locals_.getPriorYieldId(locals_.getYieldId(temp)) << ");\n";
        os << "        } while (false)" << expr.back();

        rewriter_.ReplaceText(range, os.str());
        auto macro = rewriter_.getSourceMgr().getImmediateExpansionRange(op->getLocStart());
        rewriter_.ReplaceText(SourceRange(macro.first, macro.second), "/*yield*/");
        macro = rewriter_.getSourceMgr().getImmediateExpansionRange(after_yield->getLocStart());
        rewriter_.ReplaceText(SourceRange(macro.first, macro.second), "/*from*/");
      }
      else
      {
        // "yield"

        SourceRange range(after_yield->getLocStart(), Lexer::getLocForEndOfToken(after_yield->getLocEnd(), 0, rewriter_.getSourceMgr(), rewriter_.getLangOpts()));
        std::string expr = rewriter_.getRewrittenText(range);

        int yield_point = locals_.getYieldId(op);

        std::stringstream os;
        os << "\n";
        os << "        do\n";
        os << "        {\n";
        os << "          __state = " << yield_point << ";\n";
        os << "          __unwind.__locals = nullptr;\n";
        EmitLineNumber(os, after_yield->getLocStart());
        os << "          return " << expr.substr(0, expr.length() - 1) << ";\n";
        os << "        case " << yield_point << ":\n";
        os << "          (void)0;\n";
        os << "        } while (false)" << expr.back();

        rewriter_.ReplaceText(range, os.str());
        auto macro = rewriter_.getSourceMgr().getImmediateExpansionRange(op->getLocStart());
        rewriter_.ReplaceText(SourceRange(macro.first, macro.second), "/*yield*/");
      }
    }

    return result;
  }

  bool VisitReturnStmt(ReturnStmt* stmt)
  {
    if (Expr* after_from = IsFromKeyword(stmt->getRetValue()))
    {
      // "return from"

      SourceRange range(after_from->getLocStart(), Lexer::getLocForEndOfToken(after_from->getLocEnd(), 0, rewriter_.getSourceMgr(), rewriter_.getLangOpts()));
      std::string expr = rewriter_.getRewrittenText(range);

      int yield_point = locals_.getYieldId(stmt);

      std::stringstream os;
      os << "\n";
      os << "        do\n";
      os << "        {\n";
      if (MaterializeTemporaryExpr* temp = dyn_cast<MaterializeTemporaryExpr>(after_from))
      {
        resumable_lambda_locals::iterator iter = locals_.find(temp);
        if (iter != locals_.end() && !iter->second.generator_expr.empty())
          os << "          " << iter->second.generator_expr << ";\n";
      }
      os << "          __state = " << yield_point << ";\n";
      os << "          for (;;)\n";
      os << "          {\n";
      os << "            {\n";
      os << "              auto& __g = " << expr.substr(0, expr.length() - 1) << ";\n";
      os << "              __unwind.__locals = nullptr;\n";
      os << "              auto __ret(__g());\n";
      os << "              if (__g.is_terminal())\n";
      os << "                __state = -1;\n";
      os << "              return __ret;\n";
      os << "            }\n";
      if (dyn_cast<MaterializeTemporaryExpr>(after_from))
        os << "          case " << yield_point - 1 << ":\n";
      os << "          case " << yield_point << ":\n";
      os << "            (void)0;\n";
      os << "          }\n";
      if (MaterializeTemporaryExpr* temp = dyn_cast<MaterializeTemporaryExpr>(after_from))
        os << "          __unwind_to(" << locals_.getPriorYieldId(locals_.getYieldId(temp)) << ");\n";
      os << "        } while (false)" << expr.back();

      rewriter_.ReplaceText(range, os.str());
      rewriter_.ReplaceText(stmt->getLocStart(), 6, "/*return*/");
      auto macro = rewriter_.getSourceMgr().getImmediateExpansionRange(stmt->getRetValue()->getLocStart());
      rewriter_.ReplaceText(SourceRange(macro.first, macro.second), "/*from*/");
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
    os << "    explicit __resumable_lambda_" << lambda_id_ << "_capture(__resumable_dummy_arg";
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
      std::string type = v->second.type;
      std::string name = v->second.name;
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
    os << "    void __unwind_to(int __new_state)\n";
    os << "    {\n";
    os << "      while (__state > __new_state)\n";
    os << "      {\n";
    os << "        switch (__state)\n";
    os << "        {\n";
    for (int yield_id = locals_.getLastYieldId(); yield_id > 0; --yield_id)
    {
      int prior_yield_id = locals_.getPriorYieldId(yield_id);
      os << "        case " << yield_id << ":\n";
      resumable_lambda_locals::iterator iter = locals_.find(yield_id);
      if (iter != locals_.end())
      {
        std::string name = iter->second.full_name;
        os << "          {\n";
        if (iter->second.generator_expr.empty())
        {
          os << "            typedef decltype(" << name << ") __type;\n";
          os << "            " << name << ".~__type();\n";
        }
        else if (iter == locals_.find(yield_id - 1))
        {
          os << "            __resumable_generator_destroy(&" + name + ");\n";
        }
        else
        {
          os << "            __resumable_generator_fini(&" + name + ");\n";
        }
        os << "            __state = " << prior_yield_id << ";\n";
        os << "          }\n";
      }
      if (prior_yield_id != yield_id - 1)
      {
        os << "          __state = " << prior_yield_id << ";\n";
        os << "          break;\n";
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
      os << "      ::std::is_copy_constructible<decltype(" << v->second.full_name << ")>::value &&\n";
    os << "      true };\n";
    os << "\n";
    os << "    typedef ::std::integral_constant<bool, __is_copy_constructible_v>\n";
    os << "      __is_copy_constructible __RESUMABLE_UNUSED_TYPEDEF;\n";
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
      std::string name = v->second.full_name;
      int yield_id = v->second.yield_id;

      os << "      switch (__other.__state)\n";
      os << "      {\n";
      for (int i = 0; i <= locals_.getLastYieldId(); ++i)
        if (yield_id == i || locals_.isReachable(yield_id, i))
          os << "      case " << i << ":\n";
      os << "        __resumable_local_new(__is_copy_constructible(), &" + name + ", __other." + name + ");\n";
      os << "        __state = " << yield_id << ";\n";
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
      os << "      ::std::is_move_constructible<decltype(" << v->second.full_name << ")>::value &&\n";
    os << "      true };\n";
    os << "\n";
    os << "    typedef ::std::integral_constant<bool, __is_move_constructible_v>\n";
    os << "      __is_move_constructible __RESUMABLE_UNUSED_TYPEDEF;\n";
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
      std::string name = v->second.full_name;
      int yield_id = v->second.yield_id;

      os << "      switch (__other.__state)\n";
      os << "      {\n";
      for (int i = 0; i <= locals_.getLastYieldId(); ++i)
        if (yield_id == i || locals_.isReachable(yield_id, i))
          os << "      case " << i << ":\n";
      os << "        __resumable_local_new(__is_move_constructible(), &" + name + ", static_cast<decltype(" + name + ")&&>(__other." + name + "));\n";
      os << "        __state = " << v->second.yield_id << ";\n";
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
    os << "    __resumable_lambda_" << lambda_id_ << "(__resumable_dummy_arg";
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
    os << "      __resumable_lambda_" << lambda_id_ << "_capture(__resumable_dummy_arg()";
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
    os << "\n";
    os << "    __resumable_lambda_" << lambda_id_ << "(__resumable_lambda_" << lambda_id_ << "_initializer&& __init) :\n";
    os << "      __resumable_lambda_" << lambda_id_ << "_capture(\n";
    os << "          static_cast<__resumable_lambda_" << lambda_id_ << "_capture&&>(__init.__capture))\n";
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

  void EmitWantedType(std::ostream& os)
  {
    os << "    const std::type_info& wanted_type() const noexcept\n";
    os << "    {\n";
    os << "      switch (__state)\n";
    os << "      {\n";
    for (int yield_id = 0; yield_id <= locals_.getLastYieldId(); ++yield_id)
    {
      std::string subgen = locals_.getSubGenerator(yield_id);
      if (!subgen.empty())
      {
        os << "      case " << yield_id << ":\n";
        os << "        return (" + subgen + ").wanted_type();\n";
      }
    }
    os << "      default:\n";
    os << "        return typeid(void);\n";
    os << "      }\n";
    os << "    }\n";
  }

  void EmitWanted(std::ostream& os)
  {
    os << "    void* wanted() noexcept\n";
    os << "    {\n";
    os << "      switch (__state)\n";
    os << "      {\n";
    for (int yield_id = 0; yield_id <= locals_.getLastYieldId(); ++yield_id)
    {
      std::string subgen = locals_.getSubGenerator(yield_id);
      if (!subgen.empty())
      {
        os << "      case " << yield_id << ":\n";
        os << "        return (" + subgen + ").wanted();\n";
      }
    }
    os << "      default:\n";
    os << "        return nullptr;\n";
    os << "      }\n";
    os << "    }\n";
    os << "\n";
    os << "    const void* wanted() const noexcept\n";
    os << "    {\n";
    os << "      switch (__state)\n";
    os << "      {\n";
    for (int yield_id = 0; yield_id <= locals_.getLastYieldId(); ++yield_id)
    {
      std::string subgen = locals_.getSubGenerator(yield_id);
      if (!subgen.empty())
      {
        os << "      case " << yield_id << ":\n";
        os << "        return (" + subgen + ").wanted();\n";
      }
    }
    os << "      default:\n";
    os << "        return nullptr;\n";
    os << "      }\n";
    os << "    }\n";
  }

  void EmitInPlaceGenerator(std::ostream& os)
  {
    os << "  struct __resumable_lambda_" << lambda_id_ << "_in_place\n";
    os << "  {\n";
    os << "    __resumable_lambda_" << lambda_id_ << "_in_place() {}\n";
    os << "    __resumable_lambda_" << lambda_id_ << "_in_place(const __resumable_lambda_" << lambda_id_ << "_in_place&) = delete;\n";
    os << "    __resumable_lambda_" << lambda_id_ << "_in_place(__resumable_lambda_" << lambda_id_ << "_in_place&&) = delete;\n";
    os << "    __resumable_lambda_" << lambda_id_ << "_in_place& operator=(__resumable_lambda_" << lambda_id_ << "_in_place&&) = delete;\n";
    os << "    __resumable_lambda_" << lambda_id_ << "_in_place& operator=(const __resumable_lambda_" << lambda_id_ << "_in_place&) = delete;\n";
    os << "    ~__resumable_lambda_" << lambda_id_ << "_in_place() {}\n";
    os << "\n";
    os << "    void construct(__resumable_lambda_" << lambda_id_ << "_initializer&& __init)\n";
    os << "    {\n";
    os << "      new (static_cast<void*>(&__lambda)) __resumable_lambda_" << lambda_id_ << "(\n";
    os << "          static_cast<__resumable_lambda_" << lambda_id_ << "_initializer&&>(__init));\n";
    os << "    }\n";
    os << "\n";
    os << "    void destroy()\n";
    os << "    {\n";
    os << "      __lambda.~__resumable_lambda_" << lambda_id_ << "();\n";
    os << "    }\n";
    os << "\n";
    os << "    bool is_initial() const noexcept { return __lambda.is_initial(); }\n";
    os << "    bool is_terminal() const noexcept { return __lambda.is_terminal(); }\n";
    os << "\n";
    os << "    const std::type_info& wanted_type() const noexcept\n";
    os << "    {\n";
    os << "      return __lambda.wanted_type();\n";
    os << "    }\n";
    os << "\n";
    os << "    void* wanted() noexcept\n";
    os << "    {\n";
    os << "      return __lambda.wanted();\n";
    os << "    }\n";
    os << "\n";
    os << "    const void* wanted() const noexcept\n";
    os << "    {\n";
    os << "      return __lambda.wanted();\n";
    os << "    }\n";
    os << "\n";
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
    os << "    {\n";
    os << "      return __lambda(";
    for (FunctionDecl::param_iterator p = method->param_begin(), e = method->param_end(); p != e; ++p)
    {
      if (p != method->param_begin())
        os << ",";
      os << "\n      static_cast<" << (*p)->getType().getAsString() << ">(" << (*p)->getNameAsString() << ")";
    }
    os << ");\n";
    os << "    }\n";
    os << "    union { __resumable_lambda_" << lambda_id_ << " __lambda; };\n";
    os << "  };\n";
  }

  void EmitFactory(std::ostream& os)
  {
    os << "  struct __resumable_lambda_" << lambda_id_ << "_factory\n";
    os << "  {\n";
    os << "    __resumable_lambda_" << lambda_id_ << " operator()(__resumable_dummy_arg";
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
    os << ")\n";
    os << "    {\n";
    os << "      return {\n";
    os << "        __resumable_dummy_arg()";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << ",\n";
      if (c->getCaptureKind() == LCK_This)
        os << "        __capture_arg_this";
      else if (c->isInitCapture())
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "        static_cast<__resumable_lambda_" << lambda_id_ << "_" << name << "_type&&>(__capture_arg_" << name << ")";
      }
      else
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "        __capture_arg_" << name;
      }
    }
    os << "\n";
    os << "      };\n";
    os << "    }\n";
    os << "  };\n";
    os << "\n";
    os << "  return __resumable_lambda_" << lambda_id_ << "_factory();\n";
  }

  void EmitCreation(std::ostream& os)
  {
    os << "(__resumable_dummy_arg()";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << ", ";
      if (c->getCaptureKind() == LCK_This)
        os << "this";
      else if (c->isInitCapture())
        os << rewriter_.ConvertToString(c->getCapturedVar()->getInit());
      else
        os << c->getCapturedVar()->getDeclName().getAsString();
    }
    os << ")";
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
      if (verbose)
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
    if (verbose)
      llvm::errs() << "** EndSourceFileAction for: " << mgr.getFileEntryForID(mgr.getMainFileID())->getName() << "\n";
    rewriter_.getEditBuffer(mgr.getMainFileID()).write(llvm::outs());
  }

  ASTConsumer* CreateASTConsumer(CompilerInstance& compiler, StringRef file) override
  {
    if (verbose)
      llvm::errs() << "** Creating AST consumer for: " << file << "\n";
    rewriter_.setSourceMgr(compiler.getSourceManager(), compiler.getLangOpts());
    std::string preamble = "#ifndef __RESUMABLE_PREAMBLE\n";
    preamble += "#define __RESUMABLE_PREAMBLE\n";
    preamble += "\n";
    preamble += "#include <new>\n";
    preamble += "#include <typeinfo>\n";
    preamble += "#include <type_traits>\n";
    preamble += "\n";
    preamble += "#ifdef __GNUC__\n";
    preamble += "# if ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ > 4)\n";
    preamble += "#  define __RESUMABLE_UNUSED_TYPEDEF __attribute__((__unused__))\n";
    preamble += "# endif\n";
    preamble += "#endif\n";
    preamble += "#ifndef __RESUMABLE_UNUSED_TYPEDEF\n";
    preamble += "# define __RESUMABLE_UNUSED_TYPEDEF\n";
    preamble += "#endif\n";
    preamble += "\n";
    preamble += "struct __resumable_dummy_arg {};\n";
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
    preamble += "template <class>\n";
    preamble += "struct __resumable_check { typedef void _Type; };\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "struct __resumable_generator : _T {};\n";
    preamble += "\n";
    preamble += "template <class _T, class = void>\n";
    preamble += "struct __resumable_generator_type\n";
    preamble += "{\n";
    preamble += "  typedef _T _Type;\n";
    preamble += "};\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "struct __resumable_generator_type<_T,\n";
    preamble += "  typename __resumable_check<typename _T::generator_type>::_Type>\n";
    preamble += "{\n";
    preamble += "  typedef __resumable_generator<typename _T::generator_type> _Type;\n";
    preamble += "};\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline void __resumable_generator_init(__resumable_generator<_T>* __p)\n";
    preamble += "{\n";
    preamble += "  new (static_cast<void*>(__p)) __resumable_generator<_T>;\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T, class... _Args>\n";
    preamble += "inline void __resumable_generator_construct(__resumable_generator<_T>* __p, _Args&&... __args)\n";
    preamble += "{\n";
    preamble += "  __p->construct(static_cast<_Args&&>(__args)...);\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline void __resumable_generator_destroy(__resumable_generator<_T>* __p)\n";
    preamble += "{\n";
    preamble += "  __p->destroy();\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline void __resumable_generator_fini(__resumable_generator<_T>* __p)\n";
    preamble += "{\n";
    preamble += "  __p->~_T();\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline void __resumable_generator_init(_T* __p)\n";
    preamble += "{\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T, class... _Args>\n";
    preamble += "inline void __resumable_generator_construct(_T* __p, _Args&&... __args)\n";
    preamble += "{\n";
    preamble += "  new (static_cast<void*>(__p)) _T(static_cast<_Args&&>(__args)...);\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline void __resumable_generator_destroy(_T* __p)\n";
    preamble += "{\n";
    preamble += "  __p->~_T();\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline void __resumable_generator_fini(_T* __p)\n";
    preamble += "{\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline bool is_initial(const _T& __t,\n";
    preamble += "    typename __resumable_check<decltype(__t.is_initial())>::_Type* = 0) noexcept\n";
    preamble += "{\n";
    preamble += "  return __t.is_initial();\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline bool is_terminal(const _T& __t,\n";
    preamble += "    typename __resumable_check<decltype(__t.is_terminal())>::_Type* = 0) noexcept\n";
    preamble += "{\n";
    preamble += "  return __t.is_terminal();\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline const ::std::type_info& wanted_type(const _T& __t,\n";
    preamble += "    typename __resumable_check<decltype(__t.wanted_type())>::_Type* = 0) noexcept\n";
    preamble += "{\n";
    preamble += "  return __t.wanted_type();\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline void* wanted(_T& __t,\n";
    preamble += "    typename __resumable_check<decltype(__t.wanted())>::_Type* = 0) noexcept\n";
    preamble += "{\n";
    preamble += "  return __t.wanted();\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline const void* wanted(const _T& __t,\n";
    preamble += "    typename __resumable_check<decltype(__t.wanted())>::_Type* = 0) noexcept\n";
    preamble += "{\n";
    preamble += "  return __t.wanted();\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline auto initializer(_T&& __t,\n";
    preamble += "    typename __resumable_check<typename decltype(*::std::declval<_T>())::generator_type>::_Type* = 0)\n";
    preamble += "{\n";
    preamble += "  return *static_cast<_T&&>(__t);\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T, class = void>\n";
    preamble += "struct lambda\n";
    preamble += "{\n";
    preamble += "  typedef _T type;\n";
    preamble += "};\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "struct lambda<_T,\n";
    preamble += "  typename __resumable_check<typename _T::lambda>::_Type>\n";
    preamble += "{\n";
    preamble += "  typedef typename _T::lambda type;\n";
    preamble += "};\n";
    preamble += "\n";
    preamble += "template <class _T> using lambda_t = typename lambda<_T>::type;\n";
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
    std::cerr << "Usage: resumable-pp [-p <allowed_path> ] [-v] <source> [clang args]\n";
    return 1;
  }

  int arg = 1;
  while (arg < argc && argv[arg][0] == '-')
  {
    if (argv[arg] == std::string("-p"))
    {
      ++arg;
      if (arg < argc)
        allowed_path = argv[arg];
    }
    else if (argv[arg] == std::string("-v"))
      verbose = true;
    ++arg;
  }

  std::vector<std::string> files;
  if (arg < argc)
    files.push_back(argv[arg++]);

  std::vector<std::string> args;
  args.push_back("-std=c++1y");
  for (; arg < argc; ++arg)
    args.push_back(argv[arg]);

  FixedCompilationDatabase cdb(".", args);
  ClangTool tool(cdb, files);
  return tool.run(newFrontendActionFactory<frontend_action>().get());
}
