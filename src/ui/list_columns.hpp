#pragma once

#include <string>
#include <vector>

namespace ql
{

    // Column layout for the app's lists, so they use the width of the
    // terminal they're shown on: on an 80-column terminal every list looks
    // exactly as it always has; on a wider one, columns grow and more of
    // them are shown.
    //
    // A list describes its columns once, in display order. Columns with
    // add_priority 0 make up the 80-column layout and are always shown, and
    // at 80 columns that's all there is: the list looks exactly as it always
    // has. On a wider terminal, the other columns are added and the growable
    // ones widened, one step at a time in priority order (lower first; a
    // column's add and grow steps are separate, so e.g. Name can widen before
    // any column is added). Once every column is shown and there's still
    // room, the gaps between columns widen from one space to two for
    // readability. The last column shown is never cut short: it takes
    // whatever width is left, as the last column always has (a long Remarks
    // runs to the edge of the screen).
    struct ListColumn
    {
        std::string heading;
        // Its width when first shown, and the most it may widen to (the same
        // for a fixed-width column).
        int width = 0;
        int max_width = 0;
        // 0: always shown. Otherwise shown once there's room, in this order.
        int add_priority = 0;
        // Where its widening (toward max_width) falls among the steps.
        int grow_priority = 0;
        // Its fixed width in exported files (see ExportListLayout).
        int export_width = 0;
    };

    struct ListLayout
    {
        // One per column, in the same order; 0 means not shown.
        std::vector<int> widths;
        // Spaces between columns.
        int gap = 1;
        // Cut the last column to its width too (exports), rather than
        // letting it run on as on screen.
        bool cut_last = false;
    };

    // Lays out `columns` in `available` characters, where `available_at_80`
    // is what the same list gets on an 80-column terminal, starting from
    // `base_gap` spaces between columns (widened by one when there's room to
    // spare).
    ListLayout LayOutList(const std::vector<ListColumn>& columns, int available,
                          int available_at_80, int base_gap);

    // For exported files: a fixed format, the same whatever the terminal or
    // the data, so a program can read the columns by position. Every column
    // is shown at its export_width (longer values are cut to it, the last
    // column's too), with the wider gap.
    ListLayout ExportListLayout(const std::vector<ListColumn>& columns, int base_gap);

    // One row (or, given the headings, the header line): each shown cell
    // padded or cut to its column's width, the last shown one as it is
    // (unless the layout's cut_last is set).
    std::string FormatListRow(const std::vector<std::string>& cells, const ListLayout& layout);
    std::string FormatListHeading(const std::vector<ListColumn>& columns, const ListLayout& layout);

}  // namespace ql
