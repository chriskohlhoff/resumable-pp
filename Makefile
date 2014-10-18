LLVM_CXXFLAGS := `llvm-config-3.5 --cxxflags`

LLVM_LDFLAGS := `llvm-config-3.5 --ldflags --libs --system-libs`

OS_ARCH := $(shell uname)

ifeq ($(OS_ARCH),Linux)
START_GROUP = -Wl,--start-group
END_GROUP = -Wl,--end-group
endif

CLANG_LIBS = \
	$(START_GROUP) \
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
	$(END_GROUP)

bin/resumable-pp: src/resumable-pp.cpp
	g++ -std=c++11 -Wall -Wno-strict-aliasing $(LLVM_CXXFLAGS) -o $@ $< $(CLANG_LIBS) $(LLVM_LDFLAGS)

clean:
	rm -f bin/resumable-pp
