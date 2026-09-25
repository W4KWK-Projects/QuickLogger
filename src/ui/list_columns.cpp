#include "list_columns.hpp"

#include <algorithm>
#include <cstddef>

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
                          int available_at_80, int base_gap)
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

        for (const LayoutStep& step : steps)
        {
            int room = available - UsedWidth(layout);
            const ListColumn& column = columns[step.column];
            if (step.add)
            {
                if (layout.widths[step.column] == 0 && room >= column.width + layout.gap)
                {
                    layout.widths[step.column] = column.width;
                }
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
        if (shown == columns.size() && shown > 1 &&
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

    std::string FormatListRow(const std::vector<std::string>& cells, const ListLayout& layout)
    {
        std::size_t last = LastShown(layout);
        std::string row;
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
            std::string cell = cells[i].substr(0, static_cast<std::size_t>(width));
            cell.append(static_cast<std::size_t>(width) - cell.size(), ' ');
            row += cell;
        }
        return row;
    }

    std::string FormatListHeading(const std::vector<ListColumn>& columns, const ListLayout& layout)
    {
        std::vector<std::string> headings;
        for (const ListColumn& column : columns)
        {
            headings.push_back(column.heading);
        }
        return FormatListRow(headings, layout);
    }

}  // namespace ql
