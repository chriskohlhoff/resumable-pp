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

TESTS = $(wildcard test/*.cpp)
TESTS_PP = $(TESTS:test/%.cpp=test/.pp.%.cpp)
TEST_EXES = $(TESTS:test/%.cpp=test/.%.exe)
TEST_OUTPUTS = $(TESTS:test/%.cpp=test/.%.out)
TEST_RESULTS = $(TESTS:test/%.cpp=test/.%.res)

.PHONY: test
test: $(TEST_RESULTS)

$(TESTS_PP): test/.pp.%.cpp: test/%.cpp bin/resumable-pp
	-bin/resumable-pp $< > $@ 2> /dev/null

$(TEST_EXES): test/.%.exe: test/.pp.%.cpp
	g++ -std=c++1y -Wall -Wno-return-type -o $@ $<

$(TEST_OUTPUTS): test/.%.out: test/.%.exe
	$< > $@

$(TEST_RESULTS): test/.%.res: test/.%.out
	diff $< $(subst .res,.expected,$(subst test/.,test/,$@)) | tee $@
	@echo ====== PASSED ======

clean:
	rm -f bin/resumable-pp $(TEST_EXES) $(TESTS_PP) $(TEST_OUTPUTS) $(TEST_RESULTS)
