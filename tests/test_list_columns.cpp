// Laying lists out for the terminal's width (see list_columns.hpp).

#include <string>
#include <vector>

#include "../src/ui/list_columns.hpp"
#include "test_framework.hpp"

namespace ql
{

    // A, B and D always; Wide added first, then B widened, then C added.
    static std::vector<ListColumn> SampleColumns()
    {
        return {
            {"A", 4, 4, 0, 0}, {"B", 6, 10, 0, 2}, {"Wide", 8, 8, 1, 0},
            {"C", 5, 5, 3, 0}, {"D", 3, 3, 0, 0},
        };
    }

    QL_TEST(ListLayoutAt80IsJustTheAlwaysShownColumns)
    {
        ListLayout layout = LayOutList(SampleColumns(), 40, 40, 1);
        CHECK(layout.widths == std::vector<int>({4, 6, 0, 0, 3}));
        CHECK_EQ(layout.gap, 1);
        // Even with room to spare at 80 columns.
        layout = LayOutList(SampleColumns(), 30, 40, 1);
        CHECK(layout.widths == std::vector<int>({4, 6, 0, 0, 3}));
    }

    QL_TEST(ListLayoutWidensStepByStep)
    {
        // 15 to start (4+6+3 and two gaps). Wide needs 9, so with 8 to
        // spare it's skipped and the room goes to widening B instead.
        ListLayout layout = LayOutList(SampleColumns(), 23, 15, 1);
        CHECK(layout.widths == std::vector<int>({4, 10, 0, 0, 3}));
        layout = LayOutList(SampleColumns(), 24, 15, 1);
        CHECK(layout.widths == std::vector<int>({4, 6, 8, 0, 3}));
        // Then B widens (up to 10), then C.
        layout = LayOutList(SampleColumns(), 27, 15, 1);
        CHECK(layout.widths == std::vector<int>({4, 9, 8, 0, 3}));
        layout = LayOutList(SampleColumns(), 34, 15, 1);
        CHECK(layout.widths == std::vector<int>({4, 10, 8, 5, 3}));
        CHECK_EQ(layout.gap, 1);
        // Everything shown and room for a wider gap between each: 34 + 4.
        layout = LayOutList(SampleColumns(), 38, 15, 1);
        CHECK_EQ(layout.gap, 2);
    }

    QL_TEST(ListRowsPadCutAndLeaveTheLastColumnWhole)
    {
        ListLayout layout;
        layout.widths = {3, 0, 5, 4};
        layout.gap = 2;
        CHECK_EQ(FormatListRow({"ab", "hidden", "abcdefgh", "last one runs on"}, layout),
                 std::string("ab   abcde  last one runs on"));
        CHECK_EQ(
            FormatListHeading(
                {{"#", 3, 3, 0, 0}, {"x", 1, 1, 1, 0}, {"Name", 5, 5, 0, 0}, {"Note", 4, 4, 0, 0}},
                layout),
            std::string("#    Name   Note"));
    }

    QL_TEST(ExportLayoutIsFixed)
    {
        std::vector<ListColumn> columns = {
            {"A", 4, 4, 0, 0, 5}, {"Hidden on screen", 6, 6, 9, 0, 3}, {"Last", 3, 3, 0, 0, 4}};
        ListLayout layout = ExportListLayout(columns, 1);
        CHECK(layout.widths == std::vector<int>({5, 3, 4}));
        CHECK_EQ(layout.gap, 2);
        // Every column is cut to its width, the last one too.
        CHECK_EQ(FormatListRow({"abcdefg", "hijk", "lmnopq"}, layout),
                 std::string("abcde  hij  lmno"));
        CHECK_EQ(FormatListRow({"a", "b", "c"}, layout), std::string("a      b    c   "));
    }

}  // namespace ql
