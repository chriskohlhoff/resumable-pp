#include <iostream>
#include <sstream>
#include <stack>
#include <string>
#include <utility>
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;

static int next_lambda_id_ = 0;

class lambda_visitor : public RecursiveASTVisitor<lambda_visitor>
{
public:
  lambda_visitor(Rewriter& r, LambdaExpr* expr)
    : rewriter_(r),
      lambda_expr_(expr),
      lambda_id_(next_lambda_id_++)
  {
  }

  void Go()
  {
    AnnotateAttr* attr = lambda_expr_->getCallOperator()->getAttr<AnnotateAttr>();
    if (!attr || attr->getAnnotation() != "resumable")
      return;

    CompoundStmt* body = lambda_expr_->getBody();
    SourceRange beforeBody(lambda_expr_->getLocStart(), body->getLocStart());
    SourceRange afterBody(body->getLocEnd(), lambda_expr_->getLocEnd());

    std::stringstream before;
    before << "/*BEGIN RESUMABLE LAMBDA DEFINITION*/\n\n";
    before << "[&]{\n";
    EmitThisTypedef(before);
    before << "  struct __resumable_lambda_" << lambda_id_ << "\n";
    before << "  {\n";
    before << "    int __state;\n";
    EmitCaptureMembers(before);
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

  bool VisitDeclRefExpr(DeclRefExpr* expr)
  {
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      if (c->getCaptureKind() != LCK_This)
      {
        if (c->getCapturedVar() == expr->getDecl())
        {
          SourceRange range(expr->getLocStart(), expr->getLocEnd());
          rewriter_.ReplaceText(range, "__captured_" + c->getCapturedVar()->getDeclName().getAsString());
        }
      }
    }

    return true;
  }

  bool VisitCXXThisExpr(CXXThisExpr* expr)
  {
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      if (c->getCaptureKind() == LCK_This)
      {
        if (expr->isImplicit())
        {
          rewriter_.InsertTextBefore(expr->getLocStart(), "__captured_this->");
        }
        else
        {
          SourceRange range(expr->getLocStart(), expr->getLocEnd());
          rewriter_.ReplaceText(range, "__captured_this");
        }
      }
    }

    return true;
  }

  bool VisitConditionalOperator(ConditionalOperator* op)
  {
    if (IsLambdaThisKeyword(op))
    {
      // "lambda_this" - approximation of "[]this"

      auto lambda_this = rewriter_.getSourceMgr().getImmediateExpansionRange(op->getLocStart());
      SourceRange range(lambda_this.first, lambda_this.second);

      rewriter_.ReplaceText(range, "this");
    }
    else if (Expr* after_yield = IsYieldKeyword(op))
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
    if (stmt->getRetValue())
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
    }

    return true;
  }

private:
  Expr* IsYieldKeyword(Stmt* stmt)
  {
    if (ConditionalOperator::classof(stmt))
    {
      ConditionalOperator* op = static_cast<ConditionalOperator*>(stmt);
      if (op->getLocStart().isMacroID())
      {
        if (CXXThrowExpr::classof(op->getTrueExpr()))
        {
          CXXThrowExpr* true_throw_expr = static_cast<CXXThrowExpr*>(op->getTrueExpr());
          if (IntegerLiteral::classof(true_throw_expr->getSubExpr()))
          {
            IntegerLiteral* literal = static_cast<IntegerLiteral*>(true_throw_expr->getSubExpr());
            if (literal->getValue() == 99999999)
            {
              if (CXXThrowExpr::classof(op->getFalseExpr()))
              {
                CXXThrowExpr* false_throw_expr = static_cast<CXXThrowExpr*>(op->getFalseExpr());
                return false_throw_expr->getSubExpr();
              }
            }
          }
        }
      }
    }

    return nullptr;
  }

  Expr* IsFromKeyword(Stmt* stmt)
  {
    if (CXXConstructExpr::classof(stmt))
    {
      CXXConstructExpr* expr = static_cast<CXXConstructExpr*>(stmt);
      if (expr->getNumArgs() != 1)
        return nullptr;
      stmt = expr->getArg(0);
    }

    if (ImplicitCastExpr::classof(stmt))
      stmt = static_cast<ImplicitCastExpr*>(stmt)->getSubExpr();

    if (ConditionalOperator::classof(stmt))
    {
      ConditionalOperator* op = static_cast<ConditionalOperator*>(stmt);
      if (op->getLocStart().isMacroID())
      {
        if (CXXThrowExpr::classof(op->getTrueExpr()))
        {
          CXXThrowExpr* true_throw_expr = static_cast<CXXThrowExpr*>(op->getTrueExpr());
          if (IntegerLiteral::classof(true_throw_expr->getSubExpr()))
          {
            IntegerLiteral* literal = static_cast<IntegerLiteral*>(true_throw_expr->getSubExpr());
            if (literal->getValue() == 99999998)
            {
              return op->getFalseExpr();
            }
          }
        }
      }
    }

    return nullptr;
  }

  bool IsLambdaThisKeyword(Stmt* stmt)
  {
    if (ConditionalOperator::classof(stmt))
    {
      ConditionalOperator* op = static_cast<ConditionalOperator*>(stmt);
      if (op->getLocStart().isMacroID())
      {
        if (CXXThrowExpr::classof(op->getTrueExpr()))
        {
          CXXThrowExpr* true_throw_expr = static_cast<CXXThrowExpr*>(op->getTrueExpr());
          if (IntegerLiteral::classof(true_throw_expr->getSubExpr()))
          {
            IntegerLiteral* literal = static_cast<IntegerLiteral*>(true_throw_expr->getSubExpr());
            if (literal->getValue() == 99999997)
            {
              return true;
            }
          }
        }
      }
    }

    return false;
  }

  void EmitThisTypedef(std::ostream& os)
  {
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      if (c->getCaptureKind() == LCK_This)
      {
        os << "  typedef decltype(this) __resumable_lambda_" << lambda_id_ << "_this_type;\n\n";
        return;
      }
    }
  }

  void EmitCaptureMembers(std::ostream& os)
  {
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      if (c->getCaptureKind() == LCK_This)
      {
        os << "    __resumable_lambda_" << lambda_id_ << "_this_type __captured_this;\n";
      }
      else if (c->isInitCapture())
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        std::string init = rewriter_.ConvertToString(c->getCapturedVar()->getInit());
        os << "    decltype(" << init << ")";
        if (c->getCaptureKind() == LCK_ByRef)
          os << "&";
        os << " __captured_" << name << ";\n";
      }
      else
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "    decltype(" << name << ")";
        if (c->getCaptureKind() == LCK_ByRef)
          os << "&";
        os << " __captured_" << name << ";\n";
      }
    }
  }

  void EmitConstructor(std::ostream& os)
  {
    os << "    explicit __resumable_lambda_" << lambda_id_ << "(\n";
    os << "      int /*dummy*/";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << ",\n";
      if (c->getCaptureKind() == LCK_This)
      {
        os << "      __resumable_lambda_" << lambda_id_ << "_this_type __capture_arg_this";
      }
      else if (c->isInitCapture())
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        std::string init = rewriter_.ConvertToString(c->getCapturedVar()->getInit());
        os << "      decltype(" << init << ")&& __capture_arg_" << name;
      }
      else
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "      decltype(" << name << ")& __capture_arg_" << name;
      }
    }
    os << ")\n";
    os << "    : __state(0)";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << ",\n";
      if (c->getCaptureKind() == LCK_This)
      {
        os << "      __captured_this(__capture_arg_this)";
      }
      else
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "      __captured_" << name << "(__capture_arg_" << name << ")";
      }
    }
    os << "\n";
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
    os << "  return __resumable_lambda_" << lambda_id_ << "(\n";
    os << "    0 /*dummy*/";
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
    os << "#line ";
    os << rewriter_.getSourceMgr().getExpansionLineNumber(location);
    os << " \"" << rewriter_.getSourceMgr().getFilename(location).data() << "\"";
    os << "\n";
  }

  Rewriter& rewriter_;
  LambdaExpr* lambda_expr_;
  int lambda_id_;
  int next_yield_ = 1;
};

class main_visitor : public RecursiveASTVisitor<main_visitor>
{
public:
  main_visitor(Rewriter& r)
    : rewriter_(r)
  {
  }

  bool VisitLambdaExpr(LambdaExpr* expr)
  {
    lambda_visitor(rewriter_, expr).Go();
    return true;
  }

private:
  Rewriter& rewriter_;
};

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
      visitor_.TraverseDecl(*b);
      (*b)->dump();
    }

    return true;
  }

private:
  main_visitor visitor_;
};

class action : public ASTFrontendAction
{
public:
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

int main(int argc, const char* argv[])
{
  if (argc < 2)
  {
    std::cerr << "Usage: resumable-pp <source> [clang args]\n";
    return 1;
  }

  std::vector<std::string> args;
  args.push_back("-std=c++1y");
  args.push_back("-Dresumable=__attribute__((__annotate__(\"resumable\"))) mutable");
  args.push_back("-Dyield=0?throw 99999999:throw");
  args.push_back("-Dfrom=0?throw 99999998:");
  args.push_back("-Dlambda_this=(0?throw 99999997:((void(*)())0))");
  for (int arg = 2; arg < argc; ++arg)
    args.push_back(argv[arg]);

  std::vector<std::string> files;
  files.push_back(argv[1]);

  FixedCompilationDatabase cdb(".", args);
  ClangTool tool(cdb, files);
  return tool.run(newFrontendActionFactory<action>().get());
}
