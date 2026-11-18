// test_term_buffer.cpp - TermBuffer: line splitting, control-character filtering, limits, render caps.
#include "../src/term_buffer.hpp"
#include "test_util.hpp"

using id6term::TermBuffer;

int main() {
    std::string out;

    {   // basic lines, trailing partial line is visible, CRLF counts once
        TermBuffer b;
        b.append("one\ntwo\r\nthree");
        b.render(out);
        CHECK_EQ_STR(out, "one\ntwo\nthree");
        CHECK_EQ(b.lineCount(), 3);
        b.append("-more\n");
        b.render(out);
        CHECK_EQ_STR(out, "one\ntwo\nthree-more");
    }
    {   // split across append() calls, including a "\r" "\n" pair split in two
        TermBuffer b;
        b.append("ab");
        b.append("c\r");
        b.append("\nd\n");
        b.render(out);
        CHECK_EQ_STR(out, "abc\nd");
    }
    {   // tabs expand to the next multiple of 8, other control chars and ANSI colour sequences are dropped
        TermBuffer b;
        b.append("a\tb\x01\x02\x7f" "c\x1b[31mred\x1b[0m\n");
        b.render(out);
        CHECK_EQ_STR(out, "a       bcred");
    }
    {   // a bare "\r" (progress bar style) ends the line too
        TermBuffer b;
        b.append("10%\r20%\r30%\n");
        b.render(out);
        CHECK_EQ_STR(out, "10%\n20%\n30%");
    }
    {   // maxLines: oldest lines fall off
        TermBuffer b(3, 160);
        b.append("1\n2\n3\n4\n5\n");
        b.render(out);
        CHECK_EQ_STR(out, "3\n4\n5");
    }
    {   // maxLineBytes: long lines are hard-wrapped
        TermBuffer b(10, 4);
        b.append("abcdefghij\n");
        b.render(out);
        CHECK_EQ_STR(out, "abcd\nefgh\nij");
    }
    {   // render(maxBytes) keeps the newest lines that fit, whole lines only
        TermBuffer b;
        b.append("aaaa\nbbbb\ncccc\n");
        b.render(out, 9);
        CHECK_EQ_STR(out, "bbbb\ncccc");
        b.render(out, 4);
        CHECK_EQ_STR(out, "cccc");
        b.render(out, 3);
        CHECK_EQ_STR(out, "");
    }
    {   // addLine commits a pending partial line first and bumps the generation; clear resets everything
        TermBuffer b;
        unsigned g0 = b.generation();
        b.append("partial");
        b.addLine("[exit 0]");
        b.render(out);
        CHECK_EQ_STR(out, "partial\n[exit 0]");
        CHECK(b.generation() != g0);
        b.clear();
        CHECK(b.empty());
        b.render(out);
        CHECK_EQ_STR(out, "");
    }
    return testSummary("test_term_buffer");
}
