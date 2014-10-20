#include <sstream>
#include <stack>
#include <string>
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;

class visitor : public RecursiveASTVisitor<visitor>
{
public:
  visitor(Rewriter& r)
    : rewriter_(r)
  {
  }

  bool VisitLambdaExpr(LambdaExpr* expr)
  {
    if (!expr->isMutable())
      return true;

    lambdas_.push(expr);

    CompoundStmt* body = expr->getBody();
    SourceRange beforeBody(expr->getLocStart(), body->getLocStart());
    SourceRange afterBody(body->getLocEnd(), expr->getLocEnd());

    std::stringstream before;
    before << "/*BEGIN RESUMABLE LAMBDA DEFINITION*/\n\n";
    before << "[&]{\n";
    EmitThisTypedef(before, expr);
    before << "  struct __resumable_lambda_" << counter_ << "\n";
    before << "  {\n";
    before << "    int __state;\n";
    EmitCaptureMembers(before, expr);
    before << "\n";
    EmitConstructor(before, expr);
    before << "\n";
    EmitCallOperatorDecl(before, expr);
    before << "    {\n";
    rewriter_.ReplaceText(beforeBody, before.str());

    TraverseCompoundStmt(body);

    std::stringstream after;
    after << "\n";
    after << "    }\n";
    after << "  };\n";
    EmitReturn(after, expr);
    after << "}()\n\n";
    after << "/*END RESUMABLE LAMBDA DEFINITION*/";
    rewriter_.ReplaceText(afterBody, after.str());

    lambdas_.pop();
    ++counter_;

    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr* expr)
  {
    if (!lambdas_.empty())
    {
      LambdaExpr* lambda = lambdas_.top();
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
      LambdaExpr* lambda = lambdas_.top();
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

  void EmitConstructor(std::ostream& os, LambdaExpr* expr)
  {
    os << "    explicit __resumable_lambda_" << counter_ << "(\n";
    os << "      int /*dummy*/";
    for (LambdaExpr::capture_iterator c = expr->capture_begin(), e = expr->capture_end(); c != e; ++c)
    {
      os << ",\n";
      if (c->getCaptureKind() == LCK_This)
      {
        os << "      __this_type __capture_arg_this";
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
    os << "    " << method->getReturnType().getAsString() << " operator()(";
    for (FunctionDecl::param_iterator p = method->param_begin(), e = method->param_end(); p != e; ++p)
    {
      if (p != method->param_begin())
        os << ",";
      os << "\n      " << (*p)->getType().getAsString() << " " << (*p)->getNameAsString();
    }
    os << ")\n";
  }

  void EmitReturn(std::ostream& os, LambdaExpr* expr)
  {
    os << "  return __resumable_lambda_" << counter_ << "(\n";
    os << "    0 /*dummy*/";
    for (LambdaExpr::capture_iterator c = expr->capture_begin(), e = expr->capture_end(); c != e; ++c)
    {
      os << ",\n";
      if (c->getCaptureKind() == LCK_This)
        os << "    this";
      else
        os << "    " << c->getCapturedVar()->getDeclName().getAsString();
    }
    os << "\n";
    os << "  );\n";
  }

  Rewriter& rewriter_;
  int counter_ = 0;
  std::stack<LambdaExpr*> lambdas_;
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
  static llvm::cl::OptionCategory category("Resumable Lambda Preprocessor");
  CommonOptionsParser opts(argc, argv, category);
  ClangTool tool(opts.getCompilations(), opts.getSourcePathList());
  return tool.run(newFrontendActionFactory<action>().get());
}
