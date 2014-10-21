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

struct lambda
{
  int lambda_id;
  LambdaExpr* expr;
  int next_yield;
};

class visitor : public RecursiveASTVisitor<visitor>
{
public:
  visitor(Rewriter& r)
    : rewriter_(r)
  {
  }

  bool VisitLambdaExpr(LambdaExpr* expr)
  {
    AnnotateAttr* attr = expr->getCallOperator()->getAttr<AnnotateAttr>();
    if (!attr || attr->getAnnotation() != "resumable")
      return true;

    int lambda_id = next_lambda_id_++;
    lambdas_.push(lambda{lambda_id, expr, 1});

    CompoundStmt* body = expr->getBody();
    SourceRange beforeBody(expr->getLocStart(), body->getLocStart());
    SourceRange afterBody(body->getLocEnd(), expr->getLocEnd());

    std::stringstream before;
    before << "/*BEGIN RESUMABLE LAMBDA DEFINITION*/\n\n";
    before << "[&]{\n";
    EmitThisTypedef(before, expr);
    before << "  struct __resumable_lambda_" << lambda_id << "\n";
    before << "  {\n";
    before << "    int __state;\n";
    EmitCaptureMembers(before, expr);
    before << "\n";
    EmitConstructor(before, lambda_id, expr);
    before << "\n";
    before << "    bool is_initial() const noexcept { return __state == 0; }\n";
    before << "    bool is_terminal() const noexcept { return __state == -1; }\n";
    before << "\n";
    EmitCallOperatorDecl(before, expr);
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
    rewriter_.ReplaceText(beforeBody, before.str());

    TraverseCompoundStmt(body);

    std::stringstream after;
    after << "\n";
    after << "      default: (void)0;\n";
    after << "      }\n";
    after << "    }\n";
    after << "  };\n";
    EmitReturn(after, lambda_id, expr);
    after << "}()\n\n";
    after << "/*END RESUMABLE LAMBDA DEFINITION*/";
    rewriter_.ReplaceText(afterBody, after.str());

    lambdas_.pop();

    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr* expr)
  {
    if (!lambdas_.empty())
    {
      LambdaExpr* lambda = lambdas_.top().expr;
      for (LambdaExpr::capture_iterator c = lambda->capture_begin(), e = lambda->capture_end(); c != e; ++c)
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
    }

    return true;
  }

  bool VisitCXXThisExpr(CXXThisExpr* expr)
  {
    if (!lambdas_.empty())
    {
      LambdaExpr* lambda = lambdas_.top().expr;
      for (LambdaExpr::capture_iterator c = lambda->capture_begin(), e = lambda->capture_end(); c != e; ++c)
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
    }

    return true;
  }

  bool VisitConditionalOperator(ConditionalOperator* op)
  {
    if (!lambdas_.empty())
    {
      if (op->getLocStart().isMacroID())
      {
        if (CXXThrowExpr::classof(op->getTrueExpr()))
        {
          CXXThrowExpr* throw_expr = static_cast<CXXThrowExpr*>(op->getTrueExpr());
          if (IntegerLiteral::classof(throw_expr->getSubExpr()))
          {
            IntegerLiteral* literal = static_cast<IntegerLiteral*>(throw_expr->getSubExpr());
            if (literal->getValue() == 99999999)
            {
              auto macro = rewriter_.getSourceMgr().getImmediateExpansionRange(op->getLocStart());
              SourceRange range(macro.first, macro.second);

              int yield_point = lambdas_.top().next_yield++;
              rewriter_.ReplaceText(range, "{ __state = " + std::to_string(yield_point) + "; __term.__state = 0; return");

              auto end = Lexer::findLocationAfterToken(op->getFalseExpr()->getLocEnd(), tok::semi, rewriter_.getSourceMgr(), rewriter_.getLangOpts(), true);
              rewriter_.InsertTextAfter(end, " case " + std::to_string(yield_point) + ": (void)0; }");
            }
          }
        }
      }
    }

    return true;
  }

private:
  void EmitThisTypedef(std::ostream& os, LambdaExpr* expr)
  {
    for (LambdaExpr::capture_iterator c = expr->capture_begin(), e = expr->capture_end(); c != e; ++c)
    {
      if (c->getCaptureKind() == LCK_This)
      {
        os << "  typedef decltype(this) __this_type;\n\n";
        return;
      }
    }
  }

  void EmitCaptureMembers(std::ostream& os, LambdaExpr* expr)
  {
    for (LambdaExpr::capture_iterator c = expr->capture_begin(), e = expr->capture_end(); c != e; ++c)
    {
      if (c->getCaptureKind() == LCK_This)
      {
        os << "    __this_type __captured_this;\n";
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

  void EmitConstructor(std::ostream& os, int lambda_id, LambdaExpr* expr)
  {
    os << "    explicit __resumable_lambda_" << lambda_id << "(\n";
    os << "      int /*dummy*/";
    for (LambdaExpr::capture_iterator c = expr->capture_begin(), e = expr->capture_end(); c != e; ++c)
    {
      os << ",\n";
      if (c->getCaptureKind() == LCK_This)
      {
        os << "      __this_type __capture_arg_this";
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
    for (LambdaExpr::capture_iterator c = expr->capture_begin(), e = expr->capture_end(); c != e; ++c)
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

  void EmitCallOperatorDecl(std::ostream& os, LambdaExpr* expr)
  {
    CXXMethodDecl* method = expr->getCallOperator();
    os << "    ";
    if (expr->hasExplicitResultType())
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

  void EmitReturn(std::ostream& os, int lambda_id, LambdaExpr* expr)
  {
    os << "  return __resumable_lambda_" << lambda_id << "(\n";
    os << "    0 /*dummy*/";
    for (LambdaExpr::capture_iterator c = expr->capture_begin(), e = expr->capture_end(); c != e; ++c)
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

  Rewriter& rewriter_;
  int next_lambda_id_ = 0;
  std::stack<lambda> lambdas_;
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
  visitor visitor_;
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
  for (int arg = 2; arg < argc; ++arg)
    args.push_back(argv[arg]);

  std::vector<std::string> files;
  files.push_back(argv[1]);

  FixedCompilationDatabase cdb(".", args);
  ClangTool tool(cdb, files);
  return tool.run(newFrontendActionFactory<action>().get());
}
