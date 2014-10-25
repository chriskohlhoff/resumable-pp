#include <iostream>
#include <list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
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
// Class to generate members corresponding to locals.

class resumable_lambda_local_members_codegen :
  public RecursiveASTVisitor<resumable_lambda_local_members_codegen>
{
public:
  explicit resumable_lambda_local_members_codegen(std::ostream& os)
    : output_(os),
      indent_("    ")
  {
  }

  void Generate(LambdaExpr* expr)
  {
    output_ << indent_ << "union\n";
    output_ << indent_ << "{\n";
    indent_ += "  ";
    next_scope_id_ = 0;
    mode_ = child_scopes;
    TraverseCompoundStmt(expr->getBody());
    indent_.pop_back();
    indent_.pop_back();
    output_ << indent_ << "};\n";
  }

  bool TraverseCompoundStmt(CompoundStmt* stmt)
  {
    if (mode_ == child_scopes)
    {
      int scope_id = next_scope_id_++;
      output_ << indent_ << "struct\n";
      output_ << indent_ << "{\n";
      indent_ += "  ";
      mode_ = this_scope;
      for (CompoundStmt::body_iterator b = stmt->body_begin(), e = stmt->body_end(); b != e; ++b)
      TraverseStmt(*b);
      output_ << indent_ << "union\n";
      output_ << indent_ << "{\n";
      indent_ += "  ";
      mode_ = child_scopes;
      for (CompoundStmt::body_iterator b = stmt->body_begin(), e = stmt->body_end(); b != e; ++b)
        TraverseStmt(*b);
      indent_.pop_back();
      indent_.pop_back();
      output_ << indent_ << "};\n";
      indent_.pop_back();
      indent_.pop_back();
      output_ << indent_ << "} __scope_" << scope_id << ";\n";
    }

    return true;
  }

  bool VisitVarDecl(VarDecl* decl)
  {
    if (mode_ == this_scope)
    {
      if (decl->hasLocalStorage())
      {
        std::string type = decl->getType().getAsString();
        std::string name = decl->getDeclName().getAsString();
        output_ << indent_ << type << " " << name << ";\n";
      }
    }

    return true;
  }

private:
  std::ostream& output_;
  std::string indent_;
  int next_scope_id_ = 0;
  enum { this_scope, child_scopes } mode_ = this_scope;
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
    before << "  struct __resumable_lambda_" << lambda_id_ << "_locals\n";
    before << "  {\n";
    resumable_lambda_local_members_codegen(before).Generate(lambda_expr_);
    before << "  };\n";
    before << "\n";
    before << "  struct __resumable_lambda_" << lambda_id_ << " :\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_capture,\n";
    before << "    __resumable_lambda_" << lambda_id_ << "_locals\n";
    before << "  {\n";
    before << "    int __state;\n";
    before << "\n";
    EmitConstructor(before);
    before << "\n";
    before << "    bool is_initial() const noexcept { return __state == 0; }\n";
    before << "    bool is_terminal() const noexcept { return __state == -1; }\n";
    before << "\n";
    EmitCallOperatorDecl(before);
    before << "    {\n";
    before << "      struct __term_check\n";
    before << "      {\n";
    before << "        int* __state;\n";
    before << "        ~__term_check() { if (__state) *__state = -1; }\n";
    before << "      } __term = { &__state };\n";
    before << "\n";
    before << "      switch (__state)\n";
    before << "      case 0:\n";
    before << "      {\n";
    EmitLineNumber(before, body->getLocStart());
    rewriter_.ReplaceText(beforeBody, before.str());

    TraverseCompoundStmt(body);

    std::stringstream after;
    after << "\n";
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

        int yield_point = next_yield_++;

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
        after << "            __term.__state = 0;\n";
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
        auto end = Lexer::findLocationAfterToken(op->getLocEnd(), tok::semi, rewriter_.getSourceMgr(), rewriter_.getLangOpts(), true);
        SourceRange range(yield.first, yield.second);

        int yield_point = next_yield_++;

        std::stringstream before;
        before << "\n";
        before << "        {\n";
        before << "          __state = " << yield_point << ";\n";
        before << "          __term.__state = 0;\n";
        before << "          return\n";
        EmitLineNumber(before, after_yield->getLocStart());
        rewriter_.ReplaceText(range, before.str());

        std::stringstream after;
        after << "        case " << yield_point << ":\n";
        after << "          (void)0;\n";
        after << "        }\n";
        rewriter_.InsertTextAfter(end, after.str());
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

      int yield_point = next_yield_++;

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
      after << "            __term.__state = 0;\n";
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
    if (locals_.count(expr->getDecl()))
    {
      SourceRange range(expr->getLocStart(), expr->getLocation());
      rewriter_.ReplaceText(range, locals_[expr->getDecl()]);
    }
    else if (expr->getDecl()->getType().getAsString() == "const struct __lambda_this_t")
    {
      auto yield = rewriter_.getSourceMgr().getImmediateExpansionRange(expr->getLocStart());
      SourceRange range(yield.first, yield.second);
      rewriter_.ReplaceText(range, "this");
    }

    return true;
  }

  bool TraverseCompoundStmt(CompoundStmt* stmt)
  {
    scope_ids_.push_back(next_scope_id_++);
    for (CompoundStmt::body_iterator b = stmt->body_begin(), e = stmt->body_end(); b != e; ++b)
      TraverseStmt(*b);
    scope_ids_.pop_front();

    return true;
  }

  bool VisitVarDecl(VarDecl* decl)
  {
    if (decl->hasLocalStorage())
    {
      std::string name;
      for (int scope: scope_ids_)
        name += "__scope_" + std::to_string(scope) + ".";
      name += decl->getDeclName().getAsString();

      locals_.insert(std::make_pair(decl, name));

      if (decl->hasInit())
      {
        SourceRange range(decl->getLocStart(), decl->getLocation());
        rewriter_.ReplaceText(range, name);
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
    os << "),\n";
    os << "      __state(0)\n";
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
  int next_yield_ = 1;
  static int next_lambda_id_;
  std::list<int> scope_ids_;
  int next_scope_id_ = 0;
  std::unordered_map<Decl*, std::string> locals_;
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
    std::string line = std::string("#line 1 \"") + file.data() + "\"\n";
    rewriter_.InsertText(rewriter_.getSourceMgr().getLocForStartOfFile(rewriter_.getSourceMgr().getMainFileID()), line);
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
