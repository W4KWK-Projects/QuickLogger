#include "list_columns.hpp"

#include <algorithm>
#include <cstddef>

#include <ftxui/screen/string.hpp>

namespace ql
{

    // One step of widening a list: showing a column, or growing one.
    struct LayoutStep
    {
        int priority = 0;
        std::size_t column = 0;
        bool add = false;
    };

    static bool StepComesFirst(const LayoutStep& a, const LayoutStep& b)
    {
        if (a.priority != b.priority)
        {
            return a.priority < b.priority;
        }
        return a.column < b.column;
    }

    // The width `layout` takes, counting only columns shown and the gaps
    // between them.
    static int UsedWidth(const ListLayout& layout)
    {
        int used = 0;
        int shown = 0;
        for (int width : layout.widths)
        {
            if (width > 0)
            {
                used += width;
                ++shown;
            }
        }
        return shown > 1 ? used + layout.gap * (shown - 1) : used;
    }

    // The index of the last column shown, or columns.size() if none is.
    static std::size_t LastShown(const ListLayout& layout)
    {
        std::size_t last = layout.widths.size();
        for (std::size_t i = 0; i < layout.widths.size(); ++i)
        {
            if (layout.widths[i] > 0)
            {
                last = i;
            }
        }
        return last;
    }

    ListLayout LayOutList(const std::vector<ListColumn>& columns, int available,
                          int available_at_80, int base_gap, int max_gap)
    {
        ListLayout layout;
        layout.gap = base_gap;
        std::vector<LayoutStep> steps;
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            layout.widths.push_back(columns[i].add_priority == 0 ? columns[i].width : 0);
            if (columns[i].add_priority > 0)
            {
                steps.push_back({columns[i].add_priority, i, true});
            }
            if (columns[i].max_width > columns[i].width)
            {
                steps.push_back({columns[i].grow_priority, i, false});
            }
        }
        std::sort(steps.begin(), steps.end(), StepComesFirst);

        // At 80 columns (or fewer) the list is exactly as it's always been.
        if (available <= available_at_80)
        {
            return layout;
        }

        // Once a column doesn't fit, none after it in priority is added:
        // a narrower terminal costs columns, never gains a later, smaller
        // one in the space a dropped one left.
        bool add_failed = false;
        for (const LayoutStep& step : steps)
        {
            int room = available - UsedWidth(layout);
            const ListColumn& column = columns[step.column];
            if (step.add && add_failed)
            {
                continue;
            }
            if (step.add)
            {
                int before = layout.widths[step.column];
                // Already shown, or only ever shown with the column before
                // it (which adds both).
                if (layout.widths[step.column] != 0 ||
                    (step.column > 0 && columns[step.column - 1].add_with_next))
                {
                    continue;
                }
                if (column.add_with_next && step.column + 1 < columns.size())
                {
                    const ListColumn& next = columns[step.column + 1];
                    if (layout.widths[step.column + 1] == 0 &&
                        room >= column.width + next.width + 2 * layout.gap)
                    {
                        layout.widths[step.column] = column.width;
                        layout.widths[step.column + 1] = next.width;
                    }
                }
                else if (room >= column.width + layout.gap)
                {
                    layout.widths[step.column] = column.width;
                }
                add_failed = layout.widths[step.column] == before;
            }
            else if (layout.widths[step.column] > 0 && room > 0)
            {
                int grown = std::min(column.max_width, layout.widths[step.column] + room);
                layout.widths[step.column] = std::max(layout.widths[step.column], grown);
            }
        }

        // Wider gaps once everything is shown and there's room for them.
        std::size_t shown = 0;
        for (int width : layout.widths)
        {
            shown += width > 0 ? 1 : 0;
        }
        int widest_gap = std::max(max_gap, base_gap + 1);
        while (shown == columns.size() && shown > 1 && layout.gap < widest_gap &&
               available - UsedWidth(layout) >= static_cast<int>(shown) - 1)
        {
            ++layout.gap;
        }
        return layout;
    }

    ListLayout ExportListLayout(const std::vector<ListColumn>& columns, int base_gap)
    {
        ListLayout layout;
        layout.gap = base_gap + 1;
        layout.cut_last = true;
        layout.widths.reserve(columns.size());
        for (const ListColumn& column : columns)
        {
            layout.widths.push_back(column.export_width);
        }
        return layout;
    }

    // True if every byte of `text` is printable ASCII, so each takes one
    // column: nearly every name, callsign and remark. Measuring and cutting
    // such text needs no UTF-8 decoding.
    static bool IsPlainAscii(const std::string& text)
    {
        for (char c : text)
        {
            unsigned char byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte > 0x7E)
            {
                return false;
            }
        }
        return true;
    }

    // Appends `text` to `row`, cut or padded with spaces to exactly `width`
    // columns.
    static void AppendCell(std::string* row, const std::string& text, int width)
    {
        std::size_t columns = static_cast<std::size_t>(width);
        if (IsPlainAscii(text))
        {
            std::size_t kept = std::min(text.size(), columns);
            row->append(text, 0, kept);
            row->append(columns - kept, ' ');
            return;
        }
        std::string cut = CutToWidth(text, width);
        int used = ftxui::string_width(cut);
        row->append(cut);
        row->append(static_cast<std::size_t>(std::max(width - used, 0)), ' ');
    }

    std::string FormatListRow(const std::vector<std::string>& cells, const ListLayout& layout)
    {
        std::size_t last = LastShown(layout);
        // Every column's width and the gaps between: the row's whole length
        // when it's all ASCII, so it's allocated once.
        std::size_t capacity = 0;
        for (std::size_t i = 0; i < layout.widths.size(); ++i)
        {
            capacity += static_cast<std::size_t>(layout.widths[i] + layout.gap);
        }
        if (!layout.cut_last && last < cells.size())
        {
            capacity += cells[last].size();
        }
        std::string row;
        row.reserve(capacity);
        bool first = true;
        for (std::size_t i = 0; i < layout.widths.size() && i < cells.size(); ++i)
        {
            int width = layout.widths[i];
            if (width == 0)
            {
                continue;
            }
            if (!first)
            {
                row.append(static_cast<std::size_t>(layout.gap), ' ');
            }
            first = false;
            if (i == last && !layout.cut_last)
            {
                row += cells[i];
                break;
            }
            AppendCell(&row, cells[i], width);
        }
        return row;
    }

    int TextWidth(const std::string& text)
    {
        if (IsPlainAscii(text))
        {
            return static_cast<int>(text.size());
        }
        return ftxui::string_width(text);
    }

    std::string CutToWidth(const std::string& text, int width)
    {
        if (IsPlainAscii(text))
        {
            std::size_t columns = static_cast<std::size_t>(std::max(width, 0));
            return text.size() <= columns ? text : text.substr(0, columns);
        }
        // One entry per column; a wide character's second is empty.
        std::vector<std::string> cells = ftxui::Utf8ToGlyphs(text);
        if (static_cast<int>(cells.size()) <= width)
        {
            return text;
        }
        std::string cut;
        for (int i = 0; i < width; ++i)
        {
            cut += cells[static_cast<std::size_t>(i)];
        }
        if (width > 0 && cells[static_cast<std::size_t>(width)].empty())
        {
            // The last column kept is the first half of a wide character.
            cut.resize(cut.size() - cells[static_cast<std::size_t>(width - 1)].size());
            cut += ' ';
        }
        return cut;
    }

    std::string FormatListHeading(const std::vector<ListColumn>& columns, const ListLayout& layout)
    {
        std::vector<std::string> headings;
        headings.reserve(columns.size());
        for (const ListColumn& column : columns)
        {
            headings.push_back(column.heading);
        }
        return FormatListRow(headings, layout);
    }

}  // namespace ql
