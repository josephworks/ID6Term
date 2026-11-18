# Host tests for the terminal core (TermBuffer, command line, ShellRunner): Linux g++ (WSL is fine), not qcc.
#   make -C tests check
CXX ?= g++
CXXFLAGS ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -pthread
BUILD = build

TESTS = test_term_buffer test_command_line test_shell_runner test_variant_read

.PHONY: check clean
check: $(addprefix $(BUILD)/,$(TESTS))
	@fail=0; for t in $(TESTS); do ./$(BUILD)/$$t || fail=1; done; exit $$fail

$(BUILD)/test_variant_read: test_variant_read.cpp ../src/bmw_rhmi.hpp test_util.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ test_variant_read.cpp

$(BUILD)/test_term_buffer: test_term_buffer.cpp ../src/term_buffer.hpp test_util.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ test_term_buffer.cpp

$(BUILD)/test_command_line: test_command_line.cpp ../src/command_line.hpp test_util.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ test_command_line.cpp

$(BUILD)/test_shell_runner: test_shell_runner.cpp ../src/shell_runner.cpp ../src/shell_runner.hpp ../src/term_buffer.hpp ../src/command_line.hpp test_util.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ test_shell_runner.cpp ../src/shell_runner.cpp

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
