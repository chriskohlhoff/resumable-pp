ifndef CXX
CXX = g++
endif

LLVM_CXXFLAGS := `llvm-config-3.5 --cxxflags`

LLVM_LDFLAGS := `llvm-config-3.5 --ldflags --libs --system-libs`

OS_ARCH := $(shell uname)

ifeq ($(OS_ARCH),Linux)
START_GROUP = -Wl,--start-group
END_GROUP = -Wl,--end-group
endif

ifeq ($(OS_ARCH),Darwin)
PP_CXXFLAGS = \
	-I/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/include/c++/v1 \
	-I/Applications/Xcode.app/Contents/Developer/usr/lib/llvm-gcc/4.2.1/include
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
	$(CXX) -std=c++11 -Wall -Wno-strict-aliasing -g $(LLVM_CXXFLAGS) -o $@ $< $(CLANG_LIBS) $(LLVM_LDFLAGS)

TEST_FILES = $(wildcard test/*.cpp) $(wildcard test/*.hpp)
TEST_FILES_PP = $(TEST_FILES:test/%pp=test/.pp/%pp)

TESTS = $(wildcard test/*.cpp)
TEST_EXES = $(TESTS:test/%.cpp=test/.%.exe)
TEST_OUTPUTS = $(TESTS:test/%.cpp=test/.%.out)
TEST_RESULTS = $(TESTS:test/%.cpp=test/.%.res)

.PHONY: test
test: $(TEST_FILES_PP) $(TEST_RESULTS)

$(TEST_FILES_PP): test/.pp/%pp: test/%pp bin/resumable-pp
	mkdir -p test/.pp
	bin/resumable-pp $< $(PP_CXXFLAGS) > $@

$(TEST_EXES): test/.%.exe: test/.pp/%.cpp
	$(CXX) -std=c++1y -Wall -Wno-return-type -o $@ $<

$(TEST_OUTPUTS): test/.%.out: test/.%.exe
	$< > $@

$(TEST_RESULTS): test/.%.res: test/.%.out
	@-diff $< $(subst .res,.expected,$(subst test/.,test/,$@)) > $@
	diff $< $(subst .res,.expected,$(subst test/.,test/,$@))
	@echo ====== PASSED ======

clean:
	rm -f bin/resumable-pp $(TEST_EXES) $(TESTS_PP) $(TEST_OUTPUTS) $(TEST_RESULTS)
