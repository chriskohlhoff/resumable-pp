#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
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
bool line_numbers = false;

//------------------------------------------------------------------------------
// The following code is injected at the beginning of the preprocessor input.

const char injected[] = R"-(

struct __co_yield_t
{
  constexpr __co_yield_t() {}
  template <class T> operator T() const;
  template <class T> T&& operator=(T&& t) const { return static_cast<T&&>(t); }
};

constexpr __co_yield_t __co_yield;

template <class _T>
struct __initializer : _T
{
  explicit __initializer(_T __t) : _T(static_cast<_T&&>(__t)) {}
  typedef _T lambda;
};

template <class _T> bool ready(const _T&) noexcept { return false; }
template <class _T> auto resume(_T& __t) { return __t(); }
template <class _T> __initializer<_T> lambda_initializer(_T __t) { return __initializer<_T>(static_cast<_T&&>(__t)); }
template <class _T> using initializer_lambda = typename _T::lambda;

#define resumable () __attribute__((__annotate__("resumable"))) mutable
#define co_yield for ((void)__co_yield;;) throw
#define break_resumable for ((void)__co_yield;;) throw

)-";

//------------------------------------------------------------------------------
// Helper function to check a filename.

void check_filename(const std::string& filename)
{
  if (!allowed_path.empty())
  {
    char real[PATH_MAX + 1];
    if (!realpath(filename.c_str(), real))
      exit(1);
    std::string realname(real);
    if (realname.find(allowed_path) != 0)
      exit(1);
    if (realname.find("..") != std::string::npos)
      exit(1);
    if (realname.find_first_of("\"$&'()*;<>?[\\]`{|}~ \t\r\n") != std::string::npos)
      exit(1);
    for (std::size_t i = 0; i < realname.length(); ++i)
      if (!isprint(realname[i]))
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

bool IsYieldKeyword(Stmt* stmt)
{
  if (ForStmt* for_stmt = dyn_cast<ForStmt>(stmt))
    if (for_stmt->getInit())
      if (CStyleCastExpr* cast_expr = dyn_cast<CStyleCastExpr>(for_stmt->getInit()))
        if (DeclRefExpr* decl = dyn_cast<DeclRefExpr>(cast_expr->getSubExpr()))
          if (decl->getType().getAsString() == "const struct __co_yield_t")
              return true;
  return false;
}

//------------------------------------------------------------------------------
// Class used to visit AST to find all functions that are resumable.

class resumable_function_detector :
  public RecursiveASTVisitor<resumable_function_detector>
{
public:
  explicit resumable_function_detector(SourceManager& mgr)
    : source_manager_(mgr)
  {
  }

  bool IsResumable(FunctionDecl* decl) const
  {
    return resumable_functions_.find(decl) != resumable_functions_.end();
  }

  bool IsResumableStatement(Stmt* stmt) const
  {
    return resumable_statements_.find(stmt) != resumable_statements_.end();
  }

  bool shouldVisitTemplateInstantiations() const
  {
    return true;
  }

  bool TraverseStmt(Stmt* stmt)
  {
    Stmt* prev_stmt = current_stmt_;
    current_stmt_ = stmt;
    bool result = RecursiveASTVisitor<resumable_function_detector>::TraverseStmt(stmt);
    current_stmt_ = prev_stmt;
    return result;
  }

  bool TraverseLambdaExpr(LambdaExpr* expr)
  {
    AnnotateAttr* attr = expr->getCallOperator()->getAttr<AnnotateAttr>();
    if (attr && attr->getAnnotation() == "resumable")
    {
      // A resumable lambda begins a new call stack.
      std::vector<FunctionDecl*> tmp_function_stack;
      tmp_function_stack.swap(function_stack_);
      TraverseCompoundStmt(expr->getBody());
      tmp_function_stack.swap(function_stack_);
    }
    else
    {
      function_stack_.push_back(expr->getCallOperator());
      TraverseCompoundStmt(expr->getBody());
      function_stack_.pop_back();
    }
    return true;
  }

  bool TraverseFunctionDecl(FunctionDecl* decl)
  {
    function_stack_.push_back(decl);
    bool result = RecursiveASTVisitor<resumable_function_detector>::TraverseFunctionDecl(decl);
    function_stack_.pop_back();
    return result;
  }

  bool TraverseCXXMethodDecl(CXXMethodDecl* decl)
  {
    function_stack_.push_back(decl);
    bool result = RecursiveASTVisitor<resumable_function_detector>::TraverseFunctionDecl(decl);
    function_stack_.pop_back();
    return result;
  }

  bool VisitForStmt(ForStmt* stmt)
  {
    if (!function_stack_.empty() && IsYieldKeyword(stmt))
      CheckAndAdd(function_stack_.back(), "'co_yield' or 'break_resumable' used in non-inline, non-template context");
    return true;
  }

  bool VisitCallExpr(CallExpr* expr)
  {
    if (Decl* decl = expr->getDirectCallee())
    {
      if (FunctionDecl* callee = dyn_cast<FunctionDecl>(decl->getCanonicalDecl()))
      {
        if (!function_stack_.empty())
          callers_.insert(std::make_pair(callee, function_stack_.back()));
        calling_statements_.insert(std::make_pair(callee, current_stmt_));
        return true;
      }
    }

    if (BlockExpr* block = dyn_cast<BlockExpr>(expr->getCallee()->IgnoreParenImpCasts()))
    {
      if (Decl* decl = dyn_cast<FunctionDecl>(block->getBlockDecl()))
      {
        if (FunctionDecl* callee = dyn_cast<FunctionDecl>(decl->getCanonicalDecl()))
        {
          if (!function_stack_.empty())
            callers_.insert(std::make_pair(callee, function_stack_.back()));
          calling_statements_.insert(std::make_pair(callee, current_stmt_));
        }
      }
    }

    return true;
  }

  void AnalyzeCallGraph()
  {
    std::vector<FunctionDecl*> resumables(resumable_functions_.begin(), resumable_functions_.end());
    for (auto decl : resumables)
      CheckAndAddCallers(decl);
    for (auto decl : resumables)
    {
      auto range = calling_statements_.equal_range(decl);
      for (auto iter = range.first; iter != range.second; ++iter)
        resumable_statements_.insert(iter->second);
    }
  }

private:
  void CheckAndAdd(FunctionDecl* decl, const char* error)
  {
    if (decl->isInlined() || decl->isTemplateInstantiation())
    {
      resumable_functions_.insert(decl);
      return;
    }

    if (decl->isDependentContext())
      return; // Function templates are ignored.

    llvm::errs() << decl->getLocation().printToString(source_manager_);
    llvm::errs() << ": error: " << error << "\n";
    std::exit(1);
  }

  void CheckAndAddCallers(FunctionDecl* callee)
  {
    auto range = callers_.equal_range(callee);
    for (auto iter = range.first; iter != range.second; ++iter)
    {
      if (resumable_functions_.count(iter->second) == 0)
      {
        CheckAndAdd(iter->second, "resumable function used in non-inline, non-template context");
        CheckAndAddCallers(iter->second);
      }
    }
  }

  SourceManager& source_manager_;
  std::vector<FunctionDecl*> function_stack_;
  std::unordered_set<FunctionDecl*> resumable_functions_;
  std::unordered_multimap<FunctionDecl*, FunctionDecl*> callers_;
  std::unordered_multimap<FunctionDecl*, Stmt*> calling_statements_;
  std::unordered_set<Stmt*> resumable_statements_;
  Stmt* current_stmt_ = nullptr;
};

//------------------------------------------------------------------------------
// Class to visit the lambda body and build a database of all local members.

class resumable_lambda_locals:
  public RecursiveASTVisitor<resumable_lambda_locals>
{
public:
  struct local
  {
    std::string type;
    std::string name;
    std::string full_name;
    int yield_id;
    VarDecl* decl;
  };

  typedef std::vector<int> scope_path;
  typedef std::map<scope_path, local>::iterator iterator;

  explicit resumable_lambda_locals(LambdaExpr* expr)
    : lambda_expr_(expr)
  {
  }

  void Build()
  {
    curr_scope_path_.clear();
    next_scope_id_ = 0;
    curr_yield_id_ = 0;
    curr_scope_yield_id_ = 0;
    TraverseCompoundStmt(lambda_expr_->getBody());
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

  bool isVariable(int yield_id)
  {
    auto iter = yield_is_variable_.find(yield_id);
    return iter != yield_is_variable_.end() ? iter->second : false;
  }

  bool stmtHasLocals(Stmt* stmt)
  {
    auto iter = stmt_has_locals_.find(stmt);
    return iter != stmt_has_locals_.end() ? iter->second : false;
  }

  std::string getSubGenerator(int yield_id)
  {
    auto iter = yield_to_subgen_.find(yield_id);
    return iter != yield_to_subgen_.end() ? iter->second : "";
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

    stmt_has_locals_[stmt] = curr_scope_yield_id_ != enclosing_scope_yield_id;
    curr_scope_yield_id_ = enclosing_scope_yield_id;
    curr_scope_path_.pop_back();
    next_scope_id_ = curr_scope_id + 1;

    return true;
  }

  bool TraverseForStmt(ForStmt* stmt)
  {
    if (IsYieldKeyword(stmt))
    {
      AddYieldPoint(stmt, false);
      return RecursiveASTVisitor<resumable_lambda_locals>::TraverseForStmt(stmt);
    }

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

    int enclosing_scope_yield_id_2 = curr_scope_yield_id_;
    TraverseStmt(stmt->getBody());
    stmt_has_locals_[stmt->getBody()] = curr_scope_yield_id_ != enclosing_scope_yield_id_2;

    stmt_has_locals_[stmt] = curr_scope_yield_id_ != enclosing_scope_yield_id;
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
    ptr_to_yield_[stmt] = enclosing_scope_yield_id;

    if (stmt->getCond())
      TraverseStmt(stmt->getCond());

    curr_scope_path_.push_back(next_scope_id_);
    next_scope_id_ = 0;

    int enclosing_scope_yield_id_2 = curr_scope_yield_id_;
    TraverseStmt(stmt->getBody());
    stmt_has_locals_[stmt->getBody()] = curr_scope_yield_id_ != enclosing_scope_yield_id_2;

    stmt_has_locals_[stmt] = curr_scope_yield_id_ != enclosing_scope_yield_id;
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
    ptr_to_yield_[stmt] = enclosing_scope_yield_id;

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

    stmt_has_locals_[stmt] = curr_scope_yield_id_ != enclosing_scope_yield_id;
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
      int yield_id = AddYieldPoint(decl, true);
      std::string type = decl->getType().getAsString();
      if (type.find("class ") == 0) type = type.substr(6);
      if (type.find("struct ") == 0) type = type.substr(7);
      std::string name = decl->getDeclName().getAsString();
      std::string full_name;
      for (int scope: curr_scope_path_)
        full_name += "__s" + std::to_string(scope) + ".";
      full_name += name;
      iterator iter = scope_to_local_.insert(std::make_pair(curr_scope_path_, local{type, name, full_name, yield_id, decl}));
      ptr_to_iter_[decl] = iter;

      if (decl->hasInit())
        yield_to_iter_[yield_id] = iter;
    }

    return result;
  }

  bool TraverseReturnStmt(ReturnStmt* stmt)
  {
    bool result = RecursiveASTVisitor<resumable_lambda_locals>::TraverseReturnStmt(stmt);

    return result;
  }

private:
  int AddYieldPoint(void* ptr, bool is_variable)
  {
    int yield_id = ++curr_yield_id_;
    int prior_yield_id = curr_scope_yield_id_;
    ptr_to_yield_[ptr] = yield_id;
    yield_to_prior_yield_[yield_id] = prior_yield_id;
    yield_is_variable_[yield_id] = is_variable;
    curr_scope_yield_id_ = yield_id;
    while (prior_yield_id > 0)
    {
      is_reachable_yield_.insert(std::make_pair(prior_yield_id, yield_id));
      prior_yield_id = getPriorYieldId(prior_yield_id);
    }
    return yield_id;
  }

  LambdaExpr* lambda_expr_ = nullptr;
  scope_path curr_scope_path_;
  int next_scope_id_;
  int curr_yield_id_;
  int curr_scope_yield_id_;
  std::multimap<scope_path, local> scope_to_local_;
  std::unordered_map<void*, iterator> ptr_to_iter_;
  std::unordered_map<void*, int> ptr_to_yield_;
  std::unordered_map<Stmt*, bool> stmt_has_locals_;
  std::unordered_map<int, iterator> yield_to_iter_;
  std::unordered_map<int, int> yield_to_prior_yield_;
  std::set<std::pair<int, int>> is_reachable_yield_;
  std::unordered_map<int, std::string> yield_to_subgen_;
  std::unordered_map<int, bool> yield_is_variable_;
};

//------------------------------------------------------------------------------
// This class is responsible for the main job of generating the code associated
// with a resumable lambda.

class resumable_lambda_codegen :
  public RecursiveASTVisitor<resumable_lambda_codegen>
{
public:
  resumable_lambda_codegen(Rewriter& r, resumable_function_detector& d, LambdaExpr* expr)
    : rewriter_(r),
      resumable_detector_(d),
      lambda_expr_(expr),
      lambda_id_(next_lambda_id_++),
      locals_(expr)
  {
  }

  void Generate()
  {
    AnnotateAttr* attr = lambda_expr_->getCallOperator()->getAttr<AnnotateAttr>();
    if (!attr || attr->getAnnotation() != "resumable")
      return;

    locals_.Build();
    CompoundStmt* body = lambda_expr_->getBody();
    SourceRange beforeBody(lambda_expr_->getLocStart(), body->getLocStart());
    SourceRange afterBody(body->getLocEnd(), lambda_expr_->getLocEnd());

    std::stringstream before;
    before << "/*BEGIN RESUMABLE LAMBDA DEFINITION*/\n\n";
    before << "[&]{\n";
    EmitCaptureTypedefs(before);
    before << "  struct __resumable_lambda_" << lambda_id_ << "\n";
    before << "  {\n";
    EmitCaptureDataMembers(before);
    before << "\n";
    EmitLocalsDataMembers(before);
    before << "\n";
    before << "    struct initializer\n";
    before << "    {\n";
    before << "      typedef __resumable_lambda_" << lambda_id_ << " lambda __RESUMABLE_UNUSED_TYPEDEF;\n";
    before << "      __capture_t __capture;\n";
    before << "    };\n";
    before << "\n";
    EmitConstructor(before);
    before << "\n";
    before << "    __resumable_lambda_" << lambda_id_ << "(const __resumable_lambda_" << lambda_id_ << "&) = delete;\n";
    before << "    __resumable_lambda_" << lambda_id_ << "& operator=(__resumable_lambda_" << lambda_id_ << "&&) = delete;\n";
    before << "\n";
    before << "    ~__resumable_lambda_" << lambda_id_ << "() { this->__unwind_to(-1); }\n";
    before << "\n";
    EmitLocalsDataUnwindTo(before);
    before << "\n";
    before << "    initializer operator*() && { return { static_cast<__capture_t&&>(__capture) }; }\n";
    before << "\n";
    before << "    bool ready() const noexcept { return this->__state == -1; }\n";
    before << "\n";
    before << "    auto resume()\n";
    before << "    {\n";
    before << "      __on_exit_t __on_exit{this};\n";
    before << "      switch (this->__state)\n";
    before << "        if (0)\n";
    before << "        {\n";
    for (int yield_id = 0; yield_id <= locals_.getLastYieldId(); ++yield_id)
      if (!locals_.isVariable(yield_id))
        before << "          case " << yield_id << ": goto __yield_point_" << yield_id << ";\n";
    before << "          default: (void)0;\n";
    before << "        }\n";
    before << "        else __yield_point_0:\n";
    before << "\n";
    EmitLineNumber(before, body->getLocStart());
    rewriter_.ReplaceText(beforeBody, before.str());

    TraverseCompoundStmt(body);

    std::stringstream after;
    after << "\n";
    after << "    }\n";
    after << "\n";
    after << "    auto operator()() { return resume(); }\n";
    after << "  };\n";
    after << "\n";
    EmitFactory(after);
    after << "}()";
    EmitCreation(after);
    after << "\n\n";
    EmitLineNumber(after, body->getLocEnd());
    after << "/*END RESUMABLE LAMBDA DEFINITION*/";
    rewriter_.ReplaceText(afterBody, after.str());
  }

  bool TraverseStmt(Stmt* stmt)
  {
    // TODO if (resumable_detector_.IsResumableStatement(stmt))
    return RecursiveASTVisitor<resumable_lambda_codegen>::TraverseStmt(stmt);
  }

  bool TraverseCompoundStmt(CompoundStmt* stmt)
  {
    if (stmt != lambda_expr_->getBody() && locals_.stmtHasLocals(stmt))
    {
      rewriter_.InsertTextBefore(stmt->getLocStart(), "{");
    }

    bool result = RecursiveASTVisitor<resumable_lambda_codegen>::TraverseCompoundStmt(stmt);

    if (stmt != lambda_expr_->getBody() && locals_.stmtHasLocals(stmt))
    {
      std::string unwind = "this->__unwind_to(" + std::to_string(locals_.getYieldId(stmt)) + ");}";
      auto end = Lexer::getLocForEndOfToken(stmt->getLocEnd(), 0, rewriter_.getSourceMgr(), rewriter_.getLangOpts());
      rewriter_.InsertTextAfter(end, unwind);
    }

    return result;
  }

  bool TraverseForStmt(ForStmt* stmt)
  {
    if (IsYieldKeyword(stmt))
    {
      int yield_point = locals_.getYieldId(stmt);

      // "yield" statement

      std::stringstream os;
      os << "__yield(" << yield_point << ")";
      auto macro = rewriter_.getSourceMgr().getImmediateExpansionRange(stmt->getLocStart());
      rewriter_.ReplaceText(SourceRange(macro.first, macro.second), os.str());

      return RecursiveASTVisitor<resumable_lambda_codegen>::TraverseForStmt(stmt);
    }

    if (stmt->getInit() && locals_.stmtHasLocals(stmt))
    {
      rewriter_.InsertTextBefore(stmt->getLocStart(), "{");
    }

    bool result = RecursiveASTVisitor<resumable_lambda_codegen>::TraverseForStmt(stmt);

    if (stmt->getInit() && locals_.stmtHasLocals(stmt))
    {
      std::string unwind = "this->__unwind_to(" + std::to_string(locals_.getYieldId(stmt)) + ");}";
      auto end = Lexer::getLocForEndOfToken(stmt->getBody()->getLocEnd(), 0, rewriter_.getSourceMgr(), rewriter_.getLangOpts());
      rewriter_.InsertTextAfterToken(end, unwind);
    }

    return result;
  }

  bool TraverseVarDecl(VarDecl* decl)
  {
    bool result = RecursiveASTVisitor<resumable_lambda_codegen>::TraverseVarDecl(decl);

    resumable_lambda_locals::iterator iter = locals_.find(decl);
    if (iter != locals_.end())
    {
      if (decl->hasInit())
      {
        if (CXXConstructExpr* construct_expr = dyn_cast<CXXConstructExpr>(decl->getInit()))
        {
          std::string init = "new (static_cast<void*>(&" + iter->second.full_name + ")) " + iter->second.type + "(";
          if (construct_expr->getNumArgs() > 0)
            init += rewriter_.getRewrittenText(construct_expr->getParenOrBraceRange());
          init += "), this->__state = " + std::to_string(iter->second.yield_id);
          rewriter_.ReplaceText(SourceRange(decl->getLocStart(), decl->getLocEnd()), init);
        }
        else if (ExprWithCleanups* expr = dyn_cast<ExprWithCleanups>(decl->getInit()))
        {
          if (CXXConstructExpr* construct_expr = dyn_cast<CXXConstructExpr>(expr->getSubExpr()))
          {
            if (MaterializeTemporaryExpr* temp = dyn_cast<MaterializeTemporaryExpr>(construct_expr->getArg(0)))
            {
              std::string init = "new (static_cast<void*>(&" + iter->second.full_name + ")) " + iter->second.type + "(";
              init += rewriter_.getRewrittenText(SourceRange(temp->getLocStart(), temp->getLocEnd()));
              init += "), this->__state = " + std::to_string(iter->second.yield_id);
              rewriter_.ReplaceText(SourceRange(decl->getLocStart(), decl->getLocEnd()), init);
            }
          }
        }
        else
        {
          SourceRange range(decl->getLocStart(), decl->getLocation());
          rewriter_.ReplaceText(range, iter->second.full_name);
        }
      }
      else
      {
        SourceRange range(decl->getLocStart(), decl->getLocEnd());
        rewriter_.ReplaceText(range, "this->__state = " + std::to_string(iter->second.yield_id));
      }
    }

    return result;
  }

  bool TraverseCXXThisExpr(CXXThisExpr* expr)
  {
    bool result = RecursiveASTVisitor<resumable_lambda_codegen>::TraverseCXXThisExpr(expr);

    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      if (c->getCaptureKind() == LCK_This)
      {
        if (expr->isImplicit())
        {
          rewriter_.InsertTextBefore(expr->getLocStart(), "this->__capture.__this->");
        }
        else
        {
          SourceRange range(expr->getLocStart(), expr->getLocEnd());
          rewriter_.ReplaceText(range, "this->__capture.__this");
        }
      }
    }

    return result;
  }

  bool TraverseDeclRefExpr(DeclRefExpr* expr)
  {
    bool result = RecursiveASTVisitor<resumable_lambda_codegen>::TraverseDeclRefExpr(expr);

    resumable_lambda_locals::iterator iter = locals_.find(expr->getDecl());
    if (iter != locals_.end())
    {
      rewriter_.ReplaceText(SourceRange(expr->getLocStart(), expr->getLocEnd()), iter->second.full_name);
    }
    else
    {
      for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
      {
        if (c->getCapturedVar() == expr->getDecl())
        {
          std::string var = rewriter_.getRewrittenText(SourceRange(expr->getLocStart(), expr->getLocEnd()));
          rewriter_.ReplaceText(SourceRange(expr->getLocStart(), expr->getLocEnd()), "this->__capture." + var);
          break;
        }
      }
    }

    return result;
  }

  bool TraverseCallExpr(CallExpr* expr)
  {
    FunctionDecl* callee = expr->getDirectCallee();
    if (!callee || !resumable_detector_.IsResumable(callee))
      return RecursiveASTVisitor<resumable_lambda_codegen>::TraverseCallExpr(expr);

    llvm::errs() << "Found one!\n";

    return true;
  }

  bool VisitReturnStmt(ReturnStmt* stmt)
  {
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
        os << "  typedef ::std::decay<decltype(" << init << ")>::type";
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

  void EmitCaptureDataMembers(std::ostream& os)
  {
    os << "    struct __capture_t\n";
    os << "    {\n";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      if (c->getCaptureKind() == LCK_This)
      {
        os << "      __resumable_lambda_" << lambda_id_ << "_this_type __this;\n";
      }
      else
      {
        std::string name = c->getCapturedVar()->getDeclName().getAsString();
        os << "      __resumable_lambda_" << lambda_id_ << "_" << name << "_type";
        if (c->getCaptureKind() == LCK_ByRef)
          os << "&";
        os << " " << name << ";\n";
      }
    }
    os << "    } __capture;\n";
  }

  void EmitLocalsDataMembers(std::ostream& os)
  {
    resumable_lambda_locals::scope_path curr_path;
    std::string indent = "      ";

    os << "    int __state = 0;\n";
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
      while (curr_path.size() < v->first.size())
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
    os << "      while (this->__state > __new_state)\n";
    os << "      {\n";
    os << "        switch (this->__state)\n";
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
        os << "            typedef decltype(" << name << ") __type;\n";
        os << "            " << name << ".~__type();\n";
        os << "            this->__state = " << prior_yield_id << ";\n";
        os << "            break;\n";
        os << "          }\n";
      }
      else if (prior_yield_id != yield_id - 1)
      {
        os << "          this->__state = " << prior_yield_id << ";\n";
        os << "          break;\n";
      }
    }
    os << "        case 0: default:\n";
    os << "          this->__state = -1;\n";
    os << "          break;\n";
    os << "        }\n";
    os << "      }\n";
    os << "    }\n";
    os << "\n";
    os << "    struct __on_exit_t\n";
    os << "    {\n";
    os << "      __resumable_lambda_" << lambda_id_ << "* __this;\n";
    os << "      ~__on_exit_t() { if (__this) __this->__unwind_to(-1); }\n";
    os << "    };\n";
  }

  void EmitConstructor(std::ostream& os)
  {
    os << "    __resumable_lambda_" << lambda_id_ << "(";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << (c == lambda_expr_->capture_begin() ? "\n" : ",\n");
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
    os << "      : __capture{";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << (c == lambda_expr_->capture_begin() ? "\n" : ",\n");
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
        os << "          __capture_arg_" << name;
      }
    }
    os << "}\n";
    os << "    {\n";
    os << "    }\n";
    os << "\n";
    os << "    __resumable_lambda_" << lambda_id_ << "(initializer&& __init) :\n";
    os << "      __capture(static_cast<__capture_t&&>(__init.__capture))\n";
    os << "    {\n";
    os << "    }\n";
  }

  void EmitFactory(std::ostream& os)
  {
    os << "  struct __resumable_lambda_" << lambda_id_ << "_factory\n";
    os << "  {\n";
    os << "    __resumable_lambda_" << lambda_id_ << " operator()(";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << (c == lambda_expr_->capture_begin() ? "\n" : ",\n");
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
    os << "      return {";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      os << (c == lambda_expr_->capture_begin() ? "\n" : ",\n");
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
    os << "(";
    for (LambdaExpr::capture_iterator c = lambda_expr_->capture_begin(), e = lambda_expr_->capture_end(); c != e; ++c)
    {
      if (c != lambda_expr_->capture_begin())
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
    if (line_numbers)
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
  }

  Rewriter& rewriter_;
  resumable_function_detector& resumable_detector_;
  LambdaExpr* lambda_expr_;
  int lambda_id_;
  static int next_lambda_id_;
  resumable_lambda_locals locals_;
};

int resumable_lambda_codegen::next_lambda_id_ = 0;

//------------------------------------------------------------------------------
// This class visits the AST looking for potential resumable lambdas.

class codegen_visitor : public RecursiveASTVisitor<codegen_visitor>
{
public:
  codegen_visitor(Rewriter& r, resumable_function_detector& d)
    : rewriter_(r),
      resumable_detector_(d)
  {
  }

  bool VisitLambdaExpr(LambdaExpr* expr)
  {
    resumable_lambda_codegen(rewriter_, resumable_detector_, expr).Generate();
    return true;
  }

  bool TraverseFunctionDecl(FunctionDecl* decl)
  {
    if (decl->isTemplateInstantiation())
      return true;
    return RecursiveASTVisitor<codegen_visitor>::TraverseFunctionDecl(decl);
  }

private:
  Rewriter& rewriter_;
  resumable_function_detector& resumable_detector_;
};

//------------------------------------------------------------------------------
// This class is used by the compiler to process all top level declarations.

class consumer : public ASTConsumer
{
public:
  consumer(Rewriter& r)
    : resumable_detector_(r.getSourceMgr()),
      codegen_(r, resumable_detector_)
  {
  }

  void HandleTranslationUnit(ASTContext& ctx) override
  {
    if (verbose)
      ctx.getTranslationUnitDecl()->dump();
    resumable_detector_.TraverseDecl(ctx.getTranslationUnitDecl());
    resumable_detector_.AnalyzeCallGraph();
    codegen_.TraverseDecl(ctx.getTranslationUnitDecl());
  }

private:
  resumable_function_detector resumable_detector_;
  codegen_visitor codegen_;
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
    preamble += "#include <type_traits>\n";
    preamble += "\n";
    preamble += "#if defined(__clang__)\n";
    preamble += "# if (__clang_major__ >= 7)\n";
    preamble += "#  pragma GCC diagnostic ignored \"-Wdangling-else\"\n";
    preamble += "#  pragma GCC diagnostic ignored \"-Wreturn-type\"\n";
    preamble += "#  define __RESUMABLE_UNUSED_TYPEDEF __attribute__((__unused__))\n";
    preamble += "# endif\n";
    preamble += "#elif defined(__GNUC__)\n";
    preamble += "# if ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ > 4)\n";
    preamble += "#  pragma GCC diagnostic ignored \"-Wparentheses\"\n";
    preamble += "#  pragma GCC diagnostic ignored \"-Wreturn-type\"\n";
    preamble += "#  pragma GCC diagnostic ignored \"-Wuninitialized\"\n";
    preamble += "#  define __RESUMABLE_UNUSED_TYPEDEF __attribute__((__unused__))\n";
    preamble += "# endif\n";
    preamble += "#endif\n";
    preamble += "#ifndef __RESUMABLE_UNUSED_TYPEDEF\n";
    preamble += "# define __RESUMABLE_UNUSED_TYPEDEF\n";
    preamble += "#endif\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline auto ready(const _T& __t) noexcept -> decltype(__t.ready())\n";
    preamble += "{\n";
    preamble += "  return __t.ready();\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline auto resume(_T& __t) -> decltype(__t.resume())\n";
    preamble += "{\n";
    preamble += "  return __t.resume();\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T>\n";
    preamble += "inline auto lambda_initializer(_T&& __t)\n";
    preamble += "{\n";
    preamble += "  return *static_cast<_T&&>(__t);\n";
    preamble += "}\n";
    preamble += "\n";
    preamble += "template <class _T> using initializer_lambda = typename _T::lambda;\n";
    preamble += "\n";
    preamble += "#define __yield(n) \\\n";
    preamble += "  for (this->__state = (n), __on_exit.__this = nullptr;;) \\\n";
    preamble += "    if (0) \\\n";
    preamble += "      __yield_point_ ## n: break; \\\n";
    preamble += "    else \\\n";
    preamble += "      return\n";
    preamble += "\n";
    preamble += "#define co_yield for (;;) throw\n";
    preamble += "#define break_resumable for (;;) throw\n";
    preamble += "\n";
    preamble += "#endif // __RESUMABLE_PREAMBLE\n";
    preamble += "\n";
    if (line_numbers)
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
    std::cerr << "Usage: resumable-pp [-l] [-p <allowed_path> ] [-v] <source> [clang args]\n";
    return 1;
  }

  int arg = 1;
  while (arg < argc && argv[arg][0] == '-')
  {
    if (argv[arg] == std::string("-l"))
      line_numbers = true;
    else if (argv[arg] == std::string("-p"))
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
