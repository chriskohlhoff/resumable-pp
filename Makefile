LLVM_CXXFLAGS := `llvm-config-3.5 --cxxflags`

LLVM_LDFLAGS := `llvm-config-3.5 --ldflags --libs --system-libs`

CLANG_LIBS = \
	-Wl,--start-group \
	-lclangAST \
	-lclangAnalysis \
	-lclangBasic \
	-lclangDriver \
	-lclangEdit \
	-lclangFrontend \
	-lclangFrontendTool \
	-lclangLex \
	-lclangParse \
	-lclangSema \
	-lclangEdit \
	-lclangASTMatchers \
	-lclangRewrite \
	-lclangRewriteFrontend \
	-lclangStaticAnalyzerFrontend \
	-lclangStaticAnalyzerCheckers \
	-lclangStaticAnalyzerCore \
	-lclangSerialization \
	-lclangTooling \
	-Wl,--end-group

bin/resumable-pp: src/resumable-pp.cpp
	g++ -std=c++11 -Wall -Wno-strict-aliasing $(LLVM_CXXFLAGS) -o $@ $< $(CLANG_LIBS) $(LLVM_LDFLAGS)
