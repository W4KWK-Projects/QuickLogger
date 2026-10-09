#include "pages.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "../date_utils.hpp"
#include "../gmrs_channels.hpp"
#include "../show_folder.hpp"
#include "../zmodem_send.hpp"
#include "../uls_import.hpp"
#include "chrome.hpp"
#include "handlers.hpp"
#include "mouse.hpp"
#include "notes_editor.hpp"
#include "../update_check.hpp"

namespace ql
{

    // A list that a double-click presses Enter on (see DoubleClickToEnter).
    static ftxui::Component ClickableList(AppState* state, ftxui::Component menu)
    {
        return ftxui::Make<DoubleClickToEnter>(std::move(menu), state->screen);
    }

    // Error and status lines take no room at all while there's nothing to
    // say, leaving it to the page's lists on a small terminal.
    static ftxui::Element ErrorLine(const std::string& message)
    {
        // A paragraph, so a long message wraps rather than being cut off at
        // the edge of the screen.
        return message.empty() ? ftxui::emptyElement() : ftxui::paragraph(message) | ftxui::color(kColorError);
    }

    // A command to run, a solid block on its own line, so all of it (the
    // last argument too) reads as one thing to copy. Reverse video, not
    // bold: some terminals draw bold glyphs wider (see AlignedMenuEntryTransform).
    // The block ends where the command does when it fits on a line; one too
    // wide for the terminal wraps, and then fills its rows.
    static ftxui::Element CommandBlock(const std::string& command)
    {
        if (TextWidth(command) <= FrameTerminalSize().dimx)
        {
            return ftxui::hbox({ftxui::text(command) | ftxui::inverted, ftxui::filler()});
        }
        return ftxui::paragraph(command) | ftxui::inverted;
    }

    // A message; after its first line, if it has one, comes a command to
    // run (see CommandBlock).
    static ftxui::Element StatusLine(const std::string& message)
    {
        if (message.empty())
        {
            return ftxui::emptyElement();
        }
        std::string::size_type newline = message.find('\n');
        if (newline == std::string::npos)
        {
            return ftxui::paragraph(message) | ftxui::color(kColorSuccess);
        }
        return ftxui::vbox({ftxui::paragraph(message.substr(0, newline)) | ftxui::color(kColorSuccess),
                            CommandBlock(message.substr(newline + 1))});
    }

    static ftxui::Element FieldLabel(const std::string& label)
    {
        return ftxui::text(label) | ftxui::color(kColorLabel);
    }

    // Same as FTXUI's own DefaultOptionTransform (menu.cpp) -- the "> "/"  "
    // prefix, inverted colors for the keyboard-focused entry -- minus the
    // `| bold` it applies to the selected entry. Confirmed via a user
    // screenshot: at least one real terminal (ZOC) renders bold glyphs
    // measurably wider than regular ones, so bolding a long row in a
    // column-aligned list (net-history instance list, check-in list, etc.)
    // pushed every column after the bold run out from under its header --
    // visible only on whichever row happened to be selected. The "> "
    // prefix already marks the selection; bold added no information here,
    // just an alignment bug in that terminal. Pass this as
    // `MenuOption.entries_option.transform` for any Menu whose rows must
    // stay lined up under a header.
    static ftxui::Element AlignedMenuEntryTransform(const ftxui::EntryState& state)
    {
        std::string label = (state.active ? "> " : "  ") + state.label;
        ftxui::Element element = ftxui::text(std::move(label)) | ftxui::color(kColorListRow);
        if (state.focused)
        {
            element = element | ftxui::inverted;
        }
        return element;
    }

    // FTXUI's own radio-button look (RadioboxOption::Simple) -- a filled or
    // empty circle, then the choice -- in the data color, with the chosen
    // entry bold and the keyboard-focused one inverted.
    static ftxui::Element RadioEntryTransform(const ftxui::EntryState& state)
    {
#if defined(_WIN32)
        // Windows consoles' fonts often lack the circle glyphs; FTXUI itself
        // falls back to these there.
        const char* marker = state.state ? "(*) " : "( ) ";
#else
        const char* marker = state.state ? "◉ " : "○ ";
#endif
        ftxui::Element element =
            ftxui::hbox({ftxui::text(marker), ftxui::text(state.label)}) | ftxui::color(kColorData);
        if (state.active)
        {
            element = element | ftxui::bold;
        }
        if (state.focused)
        {
            element = element | ftxui::inverted;
        }
        return element;
    }

    // A horizontal radio choice (the Settings page's Time Format): the chosen
    // entry has the filled circle and is bold, and it's inverted while the
    // choice has keyboard focus. Pair with ToggleGap as elements_infix and
    // with focused_entry bound to the same int as selected, so the focus
    // highlight and the choice can't point at different entries.
    static ftxui::Element ToggleEntryTransform(const ftxui::EntryState& state)
    {
#if defined(_WIN32)
        const char* marker = state.active ? "(*) " : "( ) ";
#else
        const char* marker = state.active ? "◉ " : "○ ";
#endif
        ftxui::Element element =
            ftxui::hbox({ftxui::text(marker), ftxui::text(state.label)}) | ftxui::color(kColorData);
        if (state.active)
        {
            element = element | ftxui::bold;
        }
        if (state.focused)
        {
            element = element | ftxui::inverted;
        }
        return element;
    }

    static ftxui::Element ToggleGap()
    {
        return ftxui::text("   ");
    }

    // Hands every key to its one child except Tab/Shift-Tab, which it leaves
    // unhandled so the page's container moves focus to the next field.
    // FTXUI's Menu otherwise takes Tab as "next entry" -- for a two-choice
    // setting like Time Format, merely tabbing past it would flip it.
    class IgnoreTab : public ftxui::ComponentBase
    {
    public:
        explicit IgnoreTab(ftxui::Component child)
        {
            Add(std::move(child));
        }

        bool OnEvent(ftxui::Event event) override
        {
            if (event == ftxui::Event::Tab || event == ftxui::Event::TabReverse)
            {
                return false;
            }
            return ftxui::ComponentBase::OnEvent(event);
        }
    };

    // A net's Partial Matching toggle (see Net::partial_match_canada): US
    // or Canada, bound to `index` (an index into
    // AppState::partial_match_labels). Left/Right change it.
    static ftxui::Component PartialMatchToggle(AppState* state, int* index)
    {
        ftxui::MenuOption option = ftxui::MenuOption::Toggle();
        option.entries_option.transform = ToggleEntryTransform;
        option.elements_infix = ToggleGap;
        option.focused_entry = index;
        return std::make_shared<IgnoreTab>(ftxui::Menu(&state->partial_match_labels, index, option));
    }

    // A net's Mode choice (one of NetModes()), bound to `index` (an index
    // into AppState::mode_labels). Left/Right change it.
    static ftxui::Component ModeToggle(AppState* state, int* index)
    {
        ftxui::MenuOption option = ftxui::MenuOption::Toggle();
        option.entries_option.transform = ToggleEntryTransform;
        option.elements_infix = ToggleGap;
        option.focused_entry = index;
        return std::make_shared<IgnoreTab>(ftxui::Menu(&state->mode_labels, index, option));
    }

    // The Service toggle's on_change (New Recurring Net, Ad Hoc Net).
    class NewNetServiceChangedHandler
    {
    public:
        explicit NewNetServiceChangedHandler(AppState* state) : state_(state) {}

        void operator()() const
        {
            SetNewNetService(state_);
        }

    private:
        AppState* state_;
    };

    // A new net's Service: Amateur Radio or GMRS (AppState::service_labels).
    // Left/Right change it, and with it which radio fields show.
    static ftxui::Component ServiceToggle(AppState* state)
    {
        ftxui::MenuOption option = ftxui::MenuOption::Toggle();
        option.entries_option.transform = ToggleEntryTransform;
        option.elements_infix = ToggleGap;
        option.focused_entry = &state->new_net_service_index;
        option.on_change = NewNetServiceChangedHandler(state);
        return std::make_shared<IgnoreTab>(ftxui::Menu(&state->service_labels, &state->new_net_service_index, option));
    }

    // A GMRS net's channel, one of GmrsChannels(): Left/Right step through
    // them (1 to 22, then 15R to 22R), wrapping around. There's no typing a
    // frequency in.
    class GmrsChannelPicker : public ftxui::ComponentBase
    {
    public:
        explicit GmrsChannelPicker(int* index) : index_(index) {}

        ftxui::Element Render() override
        {
#if defined(_WIN32)
            const char* before = "< ";
            const char* after = " >";
#else
            const char* before = "◀ ";
            const char* after = " ▶";
#endif
            const GmrsChannel& channel = GmrsChannels()[static_cast<std::size_t>(*index_)];
            ftxui::Element element =
                ftxui::text(before + DescribeGmrsChannel(channel) + after) | ftxui::color(kColorData);
            if (Focused())
            {
                element = element | ftxui::inverted | ftxui::focus;
            }
            return element;
        }

        bool OnEvent(ftxui::Event event) override
        {
            if (!Focused())
            {
                return false;
            }
            int count = static_cast<int>(GmrsChannels().size());
            if (event == ftxui::Event::ArrowRight)
            {
                *index_ = (*index_ + 1) % count;
                return true;
            }
            if (event == ftxui::Event::ArrowLeft)
            {
                *index_ = (*index_ + count - 1) % count;
                return true;
            }
            return false;
        }

        bool Focusable() const override
        {
            return true;
        }

    private:
        int* index_;
    };

    static ftxui::Element PartialMatchRow(const std::string& label, const ftxui::Component& toggle)
    {
        return ftxui::hbox({FieldLabel(label), toggle->Render()});
    }

    // The Input components for every editable Station field. Shared by the
    // New Station modal, the Edit Check-in modal, and the edit-net page's
    // saved-station form, since all three collect the same identity fields.
    // `callsign` is built separately by each caller (New Station wires
    // on_enter/on_change to it for autocomplete; Edit Check-in doesn't show it
    // at all, since callsign isn't editable there).
    struct StationFieldInputs
    {
        ftxui::Component callsign;
        ftxui::Component name;
        ftxui::Component member_id;
        ftxui::Component street_address;
        ftxui::Component city;
        ftxui::Component county;
        ftxui::Component state;
        ftxui::Component zip;
        ftxui::Component grid_square;
    };

    // InputOption shared by every single-line text field in the app: plain
    // text entry, so Enter doesn't insert a newline (FTXUI's Input default is
    // multiline, which otherwise corrupts the field and swallows the Enter
    // keypress that on_enter handlers rely on).
    // How every text field draws itself: what's typed in the data color, the
    // placeholder dimmed, and the field being edited inverted -- FTXUI's own
    // default look (InputOption::Default), but in the data color rather than
    // white. Set on every InputOption in the app.
    static ftxui::Element ColoredInputTransform(ftxui::InputState state)
    {
        ftxui::Element element = std::move(state.element) | ftxui::color(kColorData);
        if (state.is_placeholder)
        {
            element = element | ftxui::dim;
        }
        if (state.focused)
        {
            element = element | ftxui::inverted;
        }
        else if (state.hovered)
        {
            element = element | ftxui::bgcolor(ftxui::Color::GrayDark);
        }
        return element;
    }

    static ftxui::InputOption SingleLineInputOption()
    {
        ftxui::InputOption option;
        option.multiline = false;
        option.transform = ColoredInputTransform;
        return option;
    }

    // A pasted public key's options (see PublicKeyFieldHandler).
    static ftxui::InputOption PublicKeyInputOption(std::string* field)
    {
        ftxui::InputOption option = SingleLineInputOption();
        option.on_change = PublicKeyFieldHandler(field);
        return option;
    }

    // A repeater offset field's options (see OffsetFieldHandler).
    static ftxui::InputOption OffsetInputOption(std::string* field)
    {
        ftxui::InputOption option = SingleLineInputOption();
        option.on_change = OffsetFieldHandler(field);
        return option;
    }

    // A frequency field's options (see FrequencyFieldHandler).
    static ftxui::InputOption FrequencyInputOption(std::string* field)
    {
        ftxui::InputOption option = SingleLineInputOption();
        option.on_change = FrequencyFieldHandler(field);
        return option;
    }

    // Builds every StationFieldInputs field except `callsign` (the caller
    // supplies that, since it needs different wiring per form) bound to
    // `station`'s fields.
    static StationFieldInputs BuildStationFieldInputs(Station* station, ftxui::Component callsign_input)
    {
        StationFieldInputs inputs;
        inputs.callsign = std::move(callsign_input);
        inputs.name = ftxui::Input(&station->name, "Name", SingleLineInputOption());
        inputs.member_id = ftxui::Input(&station->member_id, "Member ID", SingleLineInputOption());
        inputs.street_address = ftxui::Input(&station->street_address, "Street Address", SingleLineInputOption());
        inputs.city = ftxui::Input(&station->city, "City", SingleLineInputOption());
        inputs.county = ftxui::Input(&station->county, "County", SingleLineInputOption());
        inputs.state = ftxui::Input(&station->state, "State", SingleLineInputOption());
        inputs.zip = ftxui::Input(&station->zip, "Postal Code", SingleLineInputOption());
        inputs.grid_square = ftxui::Input(&station->grid_square, "Grid Square", SingleLineInputOption());
        return inputs;
    }

    // All of `inputs`' components, in Tab order, for adding to a
    // Container::Vertical. Omits `callsign` when it's null (Edit Check-in has
    // no callsign Input at all).
    static ftxui::Components StationFieldComponents(const StationFieldInputs& inputs)
    {
        ftxui::Components components;
        if (inputs.callsign)
        {
            components.push_back(inputs.callsign);
        }
        components.push_back(inputs.name);
        components.push_back(inputs.member_id);
        components.push_back(inputs.street_address);
        components.push_back(inputs.city);
        components.push_back(inputs.county);
        components.push_back(inputs.state);
        components.push_back(inputs.zip);
        components.push_back(inputs.grid_square);
        return components;
    }

    // Renders `inputs` as labeled rows, in the same order as
    // StationFieldComponents, for embedding in a page/modal's layout.
    static ftxui::Elements StationFieldRows(const StationFieldInputs& inputs)
    {
        ftxui::Elements rows;
        if (inputs.callsign)
        {
            rows.push_back(ftxui::hbox({FieldLabel("Callsign:      "), inputs.callsign->Render()}));
        }
        rows.push_back(ftxui::hbox({FieldLabel("Name:          "), inputs.name->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("Member ID:     "), inputs.member_id->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("Street Addr:   "), inputs.street_address->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("City:          "), inputs.city->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("County:        "), inputs.county->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("State:         "), inputs.state->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("Postal Code:   "), inputs.zip->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("Grid Square:   "), inputs.grid_square->Render()}));
        return rows;
    }

    // From this terminal width up, a form's fields are drawn in two columns.
    static constexpr int kTwoColumnFormWidth = 90;

    // Appends `fields` (one row per field) to `rows`: on a terminal at least
    // kTwoColumnFormWidth wide, in two side-by-side columns (the first half
    // on the left), otherwise one under another. Only the drawing changes --
    // Tab, Up and Down still visit the fields in the same order: down the
    // left column, then down the right. So a control that comes after the
    // fields in Tab order belongs in `fields` too (at the end), not drawn
    // below them, where it would sit under the left column but be reached
    // only after the right one.
    static void AppendFormFields(const AppState* state, const ftxui::Elements& fields, ftxui::Elements* rows)
    {
        if (state->list_width < kTwoColumnFormWidth || fields.size() < 2)
        {
            rows->insert(rows->end(), fields.begin(), fields.end());
            return;
        }
        std::size_t split = (fields.size() + 1) / 2;
        ftxui::Elements left(fields.begin(), fields.begin() + static_cast<std::ptrdiff_t>(split));
        ftxui::Elements right(fields.begin() + static_cast<std::ptrdiff_t>(split), fields.end());
        rows->push_back(ftxui::hbox({
            ftxui::vbox(std::move(left)) | ftxui::xflex,
            ftxui::text("   "),
            ftxui::vbox(std::move(right)) | ftxui::xflex,
        }));
    }

    // Autocomplete matches under a callsign field, the one marked ">" being
    // what Enter picks. Drawn here rather than by an ftxui::Menu: the cursor
    // stays in the Callsign field while choosing, and a Menu only scrolls to
    // its marked row when it has the cursor itself -- so on a short screen
    // the marker could move onto rows that couldn't be seen. This list is
    // as tall as the screen allows and always scrolls the marker into view.
    static ftxui::Element MatchList(const std::string& header, const std::vector<std::string>& labels, int selected)
    {
        ftxui::Elements rows;
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            bool marked = static_cast<int>(i) == selected;
            ftxui::Element row = ftxui::text((marked ? "> " : "  ") + labels[i]) | ftxui::color(kColorListRow);
            rows.push_back(marked ? row | ftxui::focus : row);
        }
        int room = std::max(2, FrameTerminalSize().dimy - kMatchWindowOtherRows);
        int height = std::min(static_cast<int>(labels.size()), room);
        return ftxui::vbox({
            HintText("Matches: Up/Down to choose, Enter to pick the one marked >"),
            DialogFramed(ftxui::vbox({
                ColumnHeader(header),
                ftxui::vbox(std::move(rows)) | ftxui::vscroll_indicator | ftxui::yframe |
                    ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, height),
            })),
        });
    }

    // Shown before actually running `sz`/`rz` -- the transfer hijacks the
    // real terminal for its raw protocol bytes and can't show anything
    // meaningful while in flight, so the operator needs to be told what to
    // do first, with a real chance to back out via Esc instead. Which
    // operation is pending (AppState::zmodem_action) changes the wording;
    // the modal itself is otherwise identical either way. Has no
    // interactive fields of its own (F2/Enter/Esc are handled by whichever
    // page's key handler is showing it, the same way every other modal
    // here works -- see AppState::show_zmodem_confirm_modal), so it's a
    // bare Renderer over an empty Container rather than a
    // Container::Vertical with actual children. Shared by every page that
    // can trigger either direction (active-net/net-history/edit-net export,
    // net-list export, import-net receive) -- built once per page via
    // BuildZmodemConfirmModal and wrapped around that page's own view with
    // ftxui::Modal.
    class ZmodemConfirmModalRenderer
    {
    public:
        explicit ZmodemConfirmModalRenderer(AppState* state) : state_(state) {}

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            // After an export of a net or closed session with an upstream set: F3 pushes it.
            bool can_push = ExportOffersPush(state_);
            const char* push_label = "Push to upstream";
            if (state_->zmodem_action == ZmodemAction::kShowFolder || state_->zmodem_action == ZmodemAction::kPushOnly)
            {
                rows.push_back(Heading("Exported"));
                rows.push_back(DialogSeparator());
                rows.push_back(ftxui::text("Saved to:"));
                for (const std::string& path : state_->zmodem_send_paths)
                {
                    rows.push_back(ftxui::text("  " + path) | ftxui::color(kColorLabel));
                }
                rows.push_back(DialogSeparator());
                if (state_->zmodem_action == ZmodemAction::kPushOnly)
                {
                    rows.push_back(KeyHintRow({{"F2/Enter", push_label}, {"Esc", "Close"}}));
                }
                else if (can_push)
                {
                    rows.push_back(KeyHintRow({{"F2/Enter", "Show Folder"}, {"F3", push_label}, {"Esc", "Close"}}));
                }
                else
                {
                    rows.push_back(KeyHintRow({{"F2/Enter", "Show Folder"}, {"Esc", "Close"}}));
                }
                return ftxui::vbox(std::move(rows)) | ftxui::color(kColorHeading) |
                       ftxui::borderStyled(kColorDialogBorder);
            }
            rows.push_back(Heading("ZMODEM"));
            rows.push_back(DialogSeparator());
            if (state_->zmodem_action == ZmodemAction::kSend)
            {
                rows.push_back(ftxui::text("Ready to send:"));
                for (const std::string& path : state_->zmodem_send_paths)
                {
                    rows.push_back(ftxui::text("  " + path) | ftxui::color(kColorLabel));
                }
                rows.push_back(ftxui::text(""));
                rows.push_back(ftxui::text("Open your terminal's file-receive (ZMODEM) dialog now, then"));
                rows.push_back(ftxui::text("press Enter to start. Gives up after 25s if"));
                rows.push_back(ftxui::text("nothing responds."));
                // Over SSH, with an address to build it from.
                if (!state_->ssh_username.empty() && HasScpAddress(state_))
                {
                    rows.push_back(ftxui::text(""));
                    rows.push_back(ftxui::text("No ZMODEM? Esc skips it and shows the scp command."));
                }
                rows.push_back(DialogSeparator());
                if (can_push)
                {
                    rows.push_back(KeyHintRow({{"F2/Enter", "Send"}, {"F3", push_label}, {"Esc", "Skip"}}));
                }
                else
                {
                    rows.push_back(KeyHintRow({{"F2/Enter", "Send"}, {"Esc", "Skip"}}));
                }
            }
            else
            {
                rows.push_back(ftxui::text(state_->ssh_username.empty() ? "Ready to receive a file into imports/."
                                                                        : "Ready to receive a file."));
                rows.push_back(ftxui::text(""));
                rows.push_back(ftxui::text("Press Enter now to start listening, THEN start"));
                rows.push_back(ftxui::text("sending (uploading) the file from your terminal client."));
                rows.push_back(ftxui::text("Gives up after 25s if nothing arrives."));
                if (!state_->ssh_username.empty() && HasScpAddress(state_))
                {
                    rows.push_back(ftxui::text(""));
                    rows.push_back(ftxui::text("No ZMODEM? Esc shows the scp upload command."));
                }
                rows.push_back(DialogSeparator());
                rows.push_back(KeyHintRow({{"F2/Enter", "Receive"}, {"Esc", "Cancel"}}));
            }

            return ftxui::vbox(std::move(rows)) | ftxui::color(kColorHeading) | ftxui::borderStyled(kColorDialogBorder);
        }

    private:
        AppState* state_;
    };

    static ftxui::Component BuildZmodemConfirmModal(AppState* state)
    {
        ftxui::Component root = ftxui::Container::Vertical({});
        return ftxui::Renderer(root, ZmodemConfirmModalRenderer(state));
    }

    // The Session Notes window (F12): as wide as the screen allows up to 100
    // columns, and tall enough for a good paragraph, the notes scrolling
    // within it.
    class SessionNotesModalRenderer
    {
    public:
        SessionNotesModalRenderer(AppState* state, ftxui::Component editor) : state_(state), editor_(std::move(editor))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Dimensions terminal = FrameTerminalSize();
            int width = std::max(20, std::min(terminal.dimx - 4, 100));
            // Its border, heading, two separators, the hint and the keys.
            int height = std::max(3, std::min(terminal.dimy - 10, 16));
            state_->session_notes_width = width - 2;
            state_->session_notes_height = height;

            bool read_only = state_->session_notes_read_only;
            ftxui::Element notes = read_only && state_->session_notes_text.empty()
                                       ? HintText("No notes for this session.")
                                       : editor_->Render();
            return ftxui::vbox({
                       Heading(state_->session_notes_title),
                       DialogSeparator(),
                       notes | ftxui::yframe | ftxui::vscroll_indicator |
                           ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, height),
                       DialogSeparator(),
                       HintText(read_only ? "Up/Down scroll." : "Enter starts a new line. Not in the text log."),
                       KeyHintRow(read_only ? std::vector<KeyHint>{{"Esc", "Close"}}
                                            : std::vector<KeyHint>{{"F2", "Save"}, {"Esc", "Cancel"}}),
                   }) |
                   ftxui::color(kColorHeading) | ftxui::borderStyled(kColorDialogBorder) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
        }

    private:
        AppState* state_;
        ftxui::Component editor_;
    };

    static ftxui::Component BuildSessionNotesModal(AppState* state)
    {
        ftxui::Component editor = std::make_shared<NotesEditor>(
            &state->session_notes_text, &state->session_notes_cursor, &state->session_notes_read_only,
            &state->session_notes_width, &state->session_notes_height);
        ftxui::Component root = ftxui::Container::Vertical({editor});
        return ftxui::Renderer(root, SessionNotesModalRenderer(state, editor));
    }

    // ---- Picking a row by number (see RowPickAction) ------------------------

    static bool IsPicking(const AppState* state, PickList list)
    {
        return state->row_pick_action != RowPickAction::kNone && RowPickListFor(state->row_pick_action) == list;
    }

    // Check-in rows already begin with their # column, which is the number
    // to type, so they get no extra number column.
    static bool PickNumbersAlreadyShown(PickList list)
    {
        return list == PickList::kActiveCheckIns || list == PickList::kHistoryCheckIns;
    }

    // Width of the number column in pick mode: the digits of the largest
    // number.
    static int PickNumberWidth(const AppState* state)
    {
        // As made when picking started (see StartRowPick).
        return state->row_pick_number_texts.empty() ? 1 : static_cast<int>(state->row_pick_number_texts[0].size()) - 1;
    }

    // Extra spaces in front of a list's column header in pick mode, so it
    // stays over its columns once the numbers push the rows right. (A
    // Menu's rows start with a two-column "> " marker; numbered rows start
    // with the number and a space instead.)
    static std::string PickHeaderPad(const AppState* state, PickList list)
    {
        if (!IsPicking(state, list) || PickNumbersAlreadyShown(list))
        {
            return "";
        }
        return std::string(static_cast<std::size_t>(PickNumberWidth(state) + 1 - 2), ' ');
    }

    // A list's column heading over its rows, shifted right in pick mode
    // (see PickHeaderPad). Only then is a new string made for it.
    static ftxui::Element PickableColumnHeader(const AppState* state, PickList list, const std::string& heading)
    {
        std::string pad = PickHeaderPad(state, list);
        if (pad.empty())
        {
            return ColumnHeader(heading);
        }
        pad += heading;
        return ColumnHeader(pad);
    }

    // A list's rows: its Menu normally; in pick mode, every row with its
    // number beside it and the row about to be picked highlighted.
    static ftxui::Element PickableRows(const AppState* state, PickList list, const std::vector<std::string>& labels,
                                       int highlighted, const ftxui::Component& menu)
    {
        if (!IsPicking(state, list))
        {
            return menu->Render();
        }
        ftxui::Elements rows;
        rows.reserve(labels.size());
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            std::string number;
            std::string rest = labels[i];
            if (PickNumbersAlreadyShown(list))
            {
                // The number is the row's own leading # column: color just
                // those digits, behind the usual two-column gutter.
                std::string::size_type digits_end = rest.find_first_not_of("0123456789");
                digits_end = digits_end == std::string::npos ? rest.size() : digits_end;
                number = "  " + rest.substr(0, digits_end);
                rest = rest.substr(digits_end);
            }
            else if (i < state->row_pick_number_texts.size())
            {
                number = state->row_pick_number_texts[i];
            }
            else
            {
                // A row added since picking started: no number, same indent.
                number = std::string(static_cast<std::size_t>(PickNumberWidth(state) + 1), ' ');
            }
            ftxui::Element row = ftxui::hbox({
                ftxui::text(number) | ftxui::color(kColorPickNumber),
                ftxui::text(rest) | ftxui::color(kColorListRow),
            });
            if (static_cast<int>(i) == highlighted)
            {
                row = row | ftxui::inverted | ftxui::focus;
            }
            rows.push_back(row | ClickableRow(static_cast<int>(i)));
        }
        return ftxui::vbox(std::move(rows));
    }

    // The prompt under a list in pick mode, with what's been typed so far.
    static ftxui::Element PickPrompt(const AppState* state, PickList list)
    {
        if (!IsPicking(state, list))
        {
            return ftxui::emptyElement();
        }
        return ftxui::hbox({
            ftxui::text(RowPickPrompt(state) + "  ") | ftxui::bold | ftxui::color(kColorLabel),
            ftxui::text("#" + state->row_pick_digits + "_") | ftxui::bold | ftxui::color(kColorData),
        });
    }

    // The key bar while picking.
    static std::vector<KeyHint> PickKeyHints(const AppState* state)
    {
        return {{"0-9", "Number"},
                {"Enter", RowPickVerbFor(state->row_pick_action)},
                {"Up/Down", "Move"},
                {"Esc", "Cancel"}};
    }

    // The confirmation a numbered delete leads to (see RowPickAction).
    class RowDeleteConfirmModalRenderer
    {
    public:
        explicit RowDeleteConfirmModalRenderer(AppState* state) : state_(state) {}

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(ftxui::text(state_->row_delete_title) | ftxui::bold | ftxui::color(kColorDanger));
            rows.push_back(DialogSeparator());
            for (std::size_t i = 0; i < state_->row_delete_lines.size(); ++i)
            {
                ftxui::Element line = ftxui::paragraph(state_->row_delete_lines[i]);
                rows.push_back(i == 0 ? line | ftxui::bold | ftxui::color(kColorLabel)
                                      : line | ftxui::color(kColorHint));
            }
            rows.push_back(DialogSeparator());
            rows.push_back(
                KeyHintRow({{"F2/Enter", "Yes, " + RowPickVerbFor(state_->row_delete_action)}, {"Esc", "Cancel"}}));
            // Long lines wrap rather than stretching the box across the
            // whole screen.
            return ftxui::vbox(std::move(rows)) | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 64) |
                   ftxui::color(kColorDanger) | ftxui::borderStyled(kColorDialogBorder);
        }

    private:
        AppState* state_;
    };

    // Wraps `page` so the numbered-delete confirmation can pop up over it.
    static ftxui::Component WithRowDeleteConfirm(AppState* state, ftxui::Component page)
    {
        ftxui::Component modal = ftxui::Renderer(ftxui::Container::Vertical({}), RowDeleteConfirmModalRenderer(state));
        return LayeredModal(std::move(page), modal, &state->show_row_delete_confirm_modal);
    }

    // A ConfirmPrompt (see AppState::confirm_prompt): its title, its lines,
    // and the keys that answer it.
    class ConfirmPromptRenderer
    {
    public:
        explicit ConfirmPromptRenderer(AppState* state) : state_(state) {}

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(Heading(state_->confirm_prompt_title));
            rows.push_back(DialogSeparator());
            for (std::size_t i = 0; i < state_->confirm_prompt_lines.size(); ++i)
            {
                ftxui::Element line = ftxui::paragraph(state_->confirm_prompt_lines[i]);
                // "Another user has closed this net..." stands out in red.
                ftxui::Color first_color = state_->confirm_prompt == ConfirmPrompt::kSessionClosed
                                               ? ftxui::Color(kColorError)
                                               : ftxui::Color(kColorLabel);
                rows.push_back(i == 0 ? line | ftxui::bold | ftxui::color(first_color)
                                      : line | ftxui::color(kColorHint));
            }
            rows.push_back(DialogSeparator());
            if (state_->confirm_prompt == ConfirmPrompt::kResumeNet)
            {
                rows.push_back(
                    KeyHintRow({{"F2/Enter", "Resume"}, {"F3", "Close & New"}, {"F4", "View"}, {"Esc", "Cancel"}}));
            }
            else if (state_->confirm_prompt == ConfirmPrompt::kSessionClosed)
            {
                rows.push_back(KeyHintRow({{"Enter", "Recurring Nets"}}));
            }
            else if (state_->confirm_prompt == ConfirmPrompt::kImportOtherNet)
            {
                rows.push_back(KeyHintRow({{"F2/Enter", "Import Anyway"}, {"Esc", "Cancel"}}));
            }
            else if (state_->confirm_prompt == ConfirmPrompt::kPushToNet)
            {
                rows.push_back(KeyHintRow({{"F2/Enter", "Push"}, {"Esc", "Don't Push"}}));
            }
            else if (CanPushUpstream(state_))
            {
                rows.push_back(
                    KeyHintRow({{"F2/Enter", "Close Net"}, {"F3", "Close & Push"}, {"Esc", "Keep Logging"}}));
            }
            else
            {
                rows.push_back(KeyHintRow({{"F2/Enter", "Close Net"}, {"Esc", "Keep Logging"}}));
            }
            return ftxui::vbox(std::move(rows)) | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 64) |
                   ftxui::color(kColorHeading) | ftxui::borderStyled(kColorDialogBorder);
        }

    private:
        AppState* state_;
    };

    ftxui::Component BuildConfirmPrompt(AppState* state)
    {
        return ftxui::Renderer(ftxui::Container::Vertical({}), ConfirmPromptRenderer(state));
    }

    // ---- Net list page ---------------------------------------------------

    class NetListRenderer
    {
    public:
        NetListRenderer(AppState* state, ftxui::Component net_menu) : state_(state), net_menu_(std::move(net_menu)) {}

        ftxui::Element operator()() const
        {
            ftxui::Element net_list_elem =
                state_->nets.empty()
                    ? HintText(state_->view_only_user || !CanCreateNets(state_)
                                   ? "No recurring nets yet."
                                   : "No recurring nets yet. Press F2 to create one.")
                    // yframe, not frame, on every list: a row can be wider
                    // than the box (the last column isn't cut), and frame
                    // would then scroll the whole list sideways to show all
                    // of the highlighted row. yframe just clips it.
                    : PickableRows(state_, PickList::kNets, state_->net_names, state_->selected_net_index, net_menu_) |
                          ftxui::yframe | ftxui::vscroll_indicator;

            // Your call signs: the amateur one, the GMRS one, or both.
            const std::string& amateur = state_->settings.callsign;
            const std::string& gmrs = state_->settings.gmrs_callsign;
            ftxui::Element callsign_hint =
                amateur.empty() && gmrs.empty()
                    ? ftxui::text("No call sign set -- see Settings (F4)") | ftxui::color(kColorLabel)
                    : ftxui::hbox({ftxui::text("Operating as ") | ftxui::color(kColorLabel),
                                   ftxui::text(amateur) | ftxui::bold | ftxui::color(kColorData),
                                   ftxui::text(gmrs.empty()      ? ""
                                               : amateur.empty() ? "GMRS "
                                                                 : ", GMRS ") |
                                       ftxui::color(kColorLabel),
                                   ftxui::text(gmrs) | ftxui::bold | ftxui::color(kColorData),
                                   state_->view_only_user ? ftxui::text("  (view-only)") | ftxui::color(kColorLabel)
                                   : !state_->is_console_session && state_->access.level == kAccessAdmin
                                       ? ftxui::text("  (Admin)") | ftxui::color(kColorLabel)
                                   : !state_->is_console_session && state_->access.level == kAccessNetAdmin
                                       ? ftxui::text("  (Net Admin)") | ftxui::color(kColorLabel)
                                       : ftxui::emptyElement()});

            // What this user may do with the highlighted net: on a net
            // that isn't theirs (restricted-nets mode) they only watch it,
            // as a view-only user does.
            bool has_selection =
                state_->selected_net_index >= 0 && state_->selected_net_index < static_cast<int>(state_->nets.size());
            NetPower power = has_selection ? PowerOnNet(state_, state_->nets[state_->selected_net_index])
                                           : (state_->view_only_user ? NetPower::kWatch : NetPower::kManage);
            bool watch_only = power == NetPower::kWatch;

            // What F3/Enter will do with the highlighted net -- by far the
            // most used key, so it's spelled out under the list.
            bool open_session = SelectedNetHasOpenSession(state_);
            ftxui::Element open_session_hint =
                open_session
                    // In the notice color, so it stands out from the usual
                    // hint and messages under the list.
                    ? (watch_only ? NoticeText("Session open: F3/Enter to view it.")
                                  : NoticeText("Session open: F3/Enter to join it, view it, or start a new one."))
                    : state_->nets.empty()     ? ftxui::emptyElement()
                      : state_->view_only_user ? ftxui::emptyElement()
                      : watch_only             ? HintText("Not one of your nets: you can watch it when it's open.")
                                               : HintText(
                                         "Choose your net with Up/Down, then press F3 (or Enter) to log "
                                                     "it.");

            ftxui::Element content = ftxui::vbox({
                callsign_hint,
                Separator(),
                Heading("Recurring Nets"),
                Framed(state_->nets.empty() ? net_list_elem
                                            : ftxui::vbox({
                                                  PickableColumnHeader(state_, PickList::kNets, NetListHeader(state_)),
                                                  net_list_elem,
                                              })) |
                    ftxui::flex,
                open_session_hint,
                PickPrompt(state_, PickList::kNets),
                StatusLine(state_->status_message),
                ErrorLine(state_->form_error),
            });

            // Nine shortcuts -- PageChrome/BottomBar wraps onto a second line
            // only if the client's terminal is too narrow to fit them all on
            // one (see chrome.hpp).
            if (state_->row_pick_action != RowPickAction::kNone)
            {
                return PageChrome("Recurring Nets", content, PickKeyHints(state_));
            }
            if (state_->view_only_user)
            {
                return PageChrome("Recurring Nets", content,
                                  {
                                      {"F3/Enter", "View"},
                                      {"F4", "Settings"},
                                      {"F5", "AdHoc"},
                                      {"F6", "History"},
                                      {"F8", "Export"},
                                      {"F10", "Quit"},
                                  });
            }
            // Only the keys this user can use on the highlighted net.
            std::vector<KeyHint> hints;
            if (CanCreateNets(state_))
            {
                hints.push_back({"F2", "New"});
            }
            hints.push_back({"F3/Enter", watch_only ? "View" : open_session ? "Join" : "Log Net"});
            hints.push_back({"F4", "Settings"});
            hints.push_back({"F5", "AdHoc"});
            hints.push_back({"F6", "History"});
            if (power == NetPower::kManage)
            {
                hints.push_back({"F7", "Edit"});
            }
            else if (power == NetPower::kLog)
            {
                hints.push_back({"F7", "Stations"});
            }
            hints.push_back({"F8", "Export"});
            if (CanCreateNets(state_) || state_->access.HasAnyGrant())
            {
                hints.push_back({"F9", "Import"});
            }
            hints.push_back({"F10", "Quit"});
            return PageChrome("Recurring Nets", content, hints);
        }

    private:
        AppState* state_;
        ftxui::Component net_menu_;
    };

    ftxui::Component BuildNetListPage(AppState* state)
    {
        // Every list's `focused_entry` (where the highlight bar is drawn) is
        // bound to the same int as its `selected` (where PickableRows puts
        // the ">"), so moving the selection in code -- the net last logged
        // at login, a check-in just added -- moves the bar too, and the two
        // can't point at different rows.
        ftxui::MenuOption net_menu_option;
        net_menu_option.focused_entry = &state->selected_net_index;
        net_menu_option.on_enter = StartSelectedNetHandler(state);
        net_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component net_menu =
            ClickableList(state, ftxui::Menu(&state->net_names, &state->selected_net_index, net_menu_option));

        ftxui::Component root = ftxui::Container::Vertical({net_menu});
        ftxui::Component main_view = ftxui::Renderer(root, NetListRenderer(state, net_menu));
        return LayeredModal(main_view, BuildZmodemConfirmModal(state), &state->show_zmodem_confirm_modal);
    }

    // ---- Create-net page ---------------------------------------------------

    // The radio fields of New Recurring Net and Ad Hoc Net: Service, then
    // for Amateur Radio Mode, Frequency and Offset, for GMRS the Channel;
    // PL Tone either way, and Partial Matching for Amateur Radio. The
    // fields of the other service are left out of Tab order (Maybe).
    struct NewNetRadioInputs
    {
        ftxui::Component service;
        ftxui::Component mode;
        ftxui::Component frequency;
        ftxui::Component offset;
        ftxui::Component channel;
        ftxui::Component tone;
        ftxui::Component partial_match;
    };

    static NewNetRadioInputs BuildNewNetRadioInputs(AppState* state)
    {
        NewNetRadioInputs inputs;
        inputs.service = ServiceToggle(state);
        inputs.mode = ftxui::Maybe(ModeToggle(state, &state->new_net_mode_index), &state->new_net_amateur);
        inputs.frequency = ftxui::Maybe(ftxui::Input(&state->new_net_frequency, "MHz, e.g. 146.940",
                                                     FrequencyInputOption(&state->new_net_frequency)),
                                        &state->new_net_amateur);
        inputs.offset = ftxui::Maybe(
            ftxui::Input(&state->new_net_offset, "e.g. -0.6 (optional)", OffsetInputOption(&state->new_net_offset)),
            &state->new_net_amateur);
        inputs.channel =
            ftxui::Maybe(std::make_shared<GmrsChannelPicker>(&state->new_net_gmrs_channel), &state->new_net_gmrs);
        inputs.tone =
            ftxui::Input(&state->new_net_tone, "e.g. 100.0 (optional)", FrequencyInputOption(&state->new_net_tone));
        inputs.partial_match =
            ftxui::Maybe(PartialMatchToggle(state, &state->new_net_partial_match_index), &state->new_net_amateur);
        return inputs;
    }

    // A net form's radio rows, for the service it's on (`gmrs` or not).
    static void AppendNewNetRadioRows(bool gmrs, const ftxui::Component& mode, const ftxui::Component& frequency,
                                      const ftxui::Component& offset, const ftxui::Component& channel,
                                      const ftxui::Component& tone, ftxui::Elements* rows)
    {
        if (gmrs)
        {
            rows->push_back(ftxui::hbox({FieldLabel("Mode:             "), ftxui::text("FM") | ftxui::color(kColorData),
                                         HintText("  (always, on GMRS)")}));
            rows->push_back(
                ftxui::hbox({FieldLabel("Channel:          "), channel->Render(), HintText("  Left/Right")}));
        }
        else
        {
            rows->push_back(ftxui::hbox({FieldLabel("Mode:             "), mode->Render()}));
            rows->push_back(ftxui::hbox({FieldLabel("Frequency:        "), frequency->Render()}));
            rows->push_back(ftxui::hbox({FieldLabel("Offset:           "), offset->Render()}));
        }
        rows->push_back(ftxui::hbox({FieldLabel("PL Tone:          "), tone->Render()}));
    }

    class CreateNetRenderer
    {
    public:
        CreateNetRenderer(AppState* state, ftxui::Component input_name, ftxui::Component service,
                          ftxui::Component input_mode, ftxui::Component input_frequency, ftxui::Component input_offset,
                          ftxui::Component channel, ftxui::Component input_tone, ftxui::Component input_location,
                          ftxui::Component input_recurrence, ftxui::Component input_comments,
                          ftxui::Component partial_match)
            : state_(state),
              input_name_(std::move(input_name)),
              service_(std::move(service)),
              channel_(std::move(channel)),
              input_mode_(std::move(input_mode)),
              input_frequency_(std::move(input_frequency)),
              input_offset_(std::move(input_offset)),
              input_tone_(std::move(input_tone)),
              input_location_(std::move(input_location)),
              input_recurrence_(std::move(input_recurrence)),
              input_comments_(std::move(input_comments)),
              partial_match_(std::move(partial_match))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Elements rows = {
                ftxui::hbox({FieldLabel("Name:             "), input_name_->Render()}),
                ftxui::hbox({FieldLabel("Service:          "), service_->Render()}),
            };
            AppendNewNetRadioRows(state_->new_net_gmrs, input_mode_, input_frequency_, input_offset_, channel_,
                                  input_tone_, &rows);
            rows.push_back(ftxui::hbox({FieldLabel("Postal Code:      "), input_location_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Recurrence:       "), input_recurrence_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Comments:         "), input_comments_->Render()}));
            if (!state_->new_net_gmrs)
            {
                rows.push_back(PartialMatchRow("Partial Matching: ", partial_match_));
            }
            rows.push_back(ErrorLine(state_->form_error));
            ftxui::Element content = ftxui::vbox(std::move(rows));

            return PageChrome("New Recurring Net", content, {{"F2", "Save"}, {"Esc", "Cancel"}});
        }

    private:
        AppState* state_;
        ftxui::Component input_name_;
        ftxui::Component service_;
        ftxui::Component channel_;
        ftxui::Component input_mode_;
        ftxui::Component input_frequency_;
        ftxui::Component input_offset_;
        ftxui::Component input_tone_;
        ftxui::Component input_location_;
        ftxui::Component input_recurrence_;
        ftxui::Component input_comments_;
        ftxui::Component partial_match_;
    };

    ftxui::Component BuildCreateNetPage(AppState* state)
    {
        ftxui::Component input_name =
            ftxui::Input(&state->new_net_name, "e.g. Weekly Skywarn Net", SingleLineInputOption());
        NewNetRadioInputs radio = BuildNewNetRadioInputs(state);
        ftxui::InputOption location_option = SingleLineInputOption();
        location_option.on_change = ZipCodeFieldHandler(&state->new_net_location);
        ftxui::Component input_location =
            ftxui::Input(&state->new_net_location, "ZIP/postal (optional)", location_option);
        ftxui::Component input_recurrence =
            ftxui::Input(&state->new_net_recurrence, "e.g. Tuesdays 8pm ET", SingleLineInputOption());
        ftxui::Component input_comments =
            ftxui::Input(&state->new_net_comments, "Anything (optional)", SingleLineInputOption());

        ftxui::Component root = ftxui::Container::Vertical({
            input_name,
            radio.service,
            radio.mode,
            radio.frequency,
            radio.offset,
            radio.channel,
            radio.tone,
            input_location,
            input_recurrence,
            input_comments,
            radio.partial_match,
        });

        state->new_net_name_input = input_name;

        return ftxui::Renderer(root, CreateNetRenderer(state, input_name, radio.service, radio.mode, radio.frequency,
                                                       radio.offset, radio.channel, radio.tone, input_location,
                                                       input_recurrence, input_comments, radio.partial_match));
    }

    // ---- Select-role page ---------------------------------------------------

    class SelectRoleRenderer
    {
    public:
        SelectRoleRenderer(AppState* state, ftxui::Component role_radiobox)
            : state_(state), role_radiobox_(std::move(role_radiobox))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element content = ftxui::vbox({
                ftxui::hbox({ftxui::text("Logging: ") | ftxui::color(kColorLabel),
                             ftxui::text(state_->start_net.name) | ftxui::bold | ftxui::color(kColorData)}),
                Separator(),
                HintText("Select your role for this net:"),
                role_radiobox_->Render(),
                Separator(),
                RoleHint("Net Control", "runs the net and calls for check-ins."),
                RoleHint("Alternate Net Control", "backs up Net Control, ready to take over."),
                RoleHint("Logger", "records check-ins while someone else runs the net."),
                RoleHint("Viewer", "watches a session that's already open, changing nothing."),
                HintParagraph("Except as a Viewer, you're logged as check-in #1, with your role."),
                ErrorLine(state_->form_error),
            });

            return PageChrome("Select Your Role", content, {{"F2/Enter", "Continue"}, {"Esc", "Back"}});
        }

    private:
        // One role's line in the list of what each role does.
        static ftxui::Element RoleHint(const std::string& role, const std::string& what)
        {
            return ftxui::hbox({ftxui::text(role + ": ") | ftxui::color(kColorLabel), HintText(what)});
        }

        AppState* state_;
        ftxui::Component role_radiobox_;
    };

    ftxui::Component BuildSelectRolePage(AppState* state)
    {
        // `focused_entry` is bound to the same variable as `selected` so
        // arrow-key movement commits immediately (Radiobox otherwise only
        // writes `hovered_` into `*selected` on an explicit Space/Enter --
        // see RadioboxBase::OnEvent) -- necessary so SelectRoleKeyHandler can
        // treat a bare Enter as "continue with whatever's currently
        // highlighted" (see below) without an extra hover-commit step first.
        ftxui::RadioboxOption role_option;
        role_option.entries = &state->role_labels;
        role_option.selected = &state->selected_role_index;
        role_option.focused_entry = &state->selected_role_index;
        role_option.transform = RadioEntryTransform;
        ftxui::Component role_radiobox = ftxui::Radiobox(role_option);

        ftxui::Component root = ftxui::Container::Vertical({role_radiobox});
        return ftxui::Renderer(root, SelectRoleRenderer(state, role_radiobox));
    }

    // ---- Enter-callsign page ---------------------------------------------------

    class EnterCallsignRenderer
    {
    public:
        EnterCallsignRenderer(AppState* state, ftxui::Component input_callsign)
            : state_(state), input_callsign_(std::move(input_callsign))
        {
        }

        ftxui::Element operator()() const
        {
            const std::string& role_label = state_->role_labels[state_->selected_role_index];

            ftxui::Element content = ftxui::vbox({
                ftxui::hbox({ftxui::text("Role: ") | ftxui::color(kColorLabel),
                             ftxui::text(role_label) | ftxui::bold | ftxui::color(kColorData)}),
                Separator(),
                ftxui::hbox({FieldLabel("Callsign: "), input_callsign_->Render()}),
                ErrorLine(state_->form_error),
            });

            return PageChrome("Enter Callsign", content, {{"F2/Enter", "Start Log"}, {"Esc", "Back"}});
        }

    private:
        AppState* state_;
        ftxui::Component input_callsign_;
    };

    ftxui::Component BuildEnterCallsignPage(AppState* state)
    {
        ftxui::InputOption input_option = SingleLineInputOption();
        input_option.on_enter = OperatorCallsignSubmitHandler(state);
        input_option.on_change = UppercaseFieldHandler(&state->operator_callsign);
        ftxui::Component input_callsign = ftxui::Input(&state->operator_callsign, "", input_option);

        ftxui::Component root = ftxui::Container::Vertical({input_callsign});
        return ftxui::Renderer(root, EnterCallsignRenderer(state, input_callsign));
    }

    // ---- Active-net page ---------------------------------------------------

    class ActiveNetRenderer
    {
    public:
        ActiveNetRenderer(AppState* state, ftxui::Component check_in_menu)
            : state_(state), check_in_menu_(std::move(check_in_menu))
        {
        }

        ftxui::Element operator()() const
        {
            const std::string& role_label = state_->role_short_labels[state_->selected_role_index];

            ftxui::Element check_in_list =
                state_->active_display_rows.empty()
                    ? HintText("No check-ins yet.")
                    : PickableRows(state_, PickList::kActiveCheckIns, state_->active_display_rows,
                                   state_->selected_check_in_index, check_in_menu_) |
                          ftxui::yframe | ftxui::vscroll_indicator;

            const std::string& started = StartedText();

            ftxui::Elements info = {
                ftxui::text("Date: ") | ftxui::color(kColorLabel),
                ftxui::text(started) | ftxui::color(kColorHeading),
                ftxui::text("   Role: ") | ftxui::color(kColorLabel),
                ftxui::text(role_label) | ftxui::color(kColorData),
                ftxui::text("   Callsign: ") | ftxui::color(kColorLabel),
                ftxui::text(state_->operator_callsign) | ftxui::bold | ftxui::color(kColorData),
            };
            // The net's frequency, offset and tone, only when the line has
            // room for them (a wide terminal): at 80 columns it's nearly full.
            std::size_t used = 6 + started.size() + 9 + role_label.size() + 13 + state_->operator_callsign.size();
            const std::string& radio = state_->active_net_radio;
            if (!radio.empty() && used + 3 + radio.size() <= static_cast<std::size_t>(state_->list_width))
            {
                info.push_back(ftxui::text("   " + radio) | ftxui::color(kColorData));
            }
            ftxui::Element info_line = ftxui::hbox(std::move(info));

            ftxui::Element content = ftxui::vbox({
                info_line,
                Separator(),
                Framed(ftxui::vbox({
                    ColumnHeader(CheckInListHeader(state_->list_width)),
                    check_in_list,
                })) |
                    ftxui::flex,
                IsPicking(state_, PickList::kActiveCheckIns)
                    ? PickPrompt(state_, PickList::kActiveCheckIns)
                    : (state_->show_new_station_modal || state_->show_edit_checkin_modal ? ftxui::text("")
                       : state_->viewing_only ? HintText("Watching as a Viewer: nothing can be changed here. Esc "
                                                         "leaves.")
                                              : HintText("F3 edits and F5 deletes a check-in by its #; Enter edits "
                                                         "the highlighted one.")),
                StatusLine(state_->status_message),
                // A check-in window shows its own errors.
                state_->show_new_station_modal || state_->show_edit_checkin_modal ? ftxui::emptyElement()
                                                                                  : ErrorLine(state_->form_error),
            });

            std::string page_title = state_->active_net_name.empty() ? "Active Net" : state_->active_net_name;
            // F3/F4/F5 only do anything when no modal is open (see
            // ActiveNetKeyHandler) -- showing them while one is up would be a
            // hint for a key that currently does nothing, so they're hidden
            // then, and F2 (context-sensitive) is relabeled for whichever
            // modal has focus instead.
            std::vector<KeyHint> hints;
            if (state_->row_pick_action != RowPickAction::kNone)
            {
                hints = PickKeyHints(state_);
            }
            else if (state_->show_edit_checkin_modal)
            {
                hints = {{"F2", "Save"}, {"F4", "Remarks"}, {"F5", "Comment"}, {"F6", "Role"}, {"Esc", "Cancel"}};
            }
            else if (state_->show_new_station_modal)
            {
                hints = {{"F2", "Log & Next"}, {"F3", "Log & Close"}, {"F4", "Remarks"},
                         {"F5", "Comment"},    {"F6", "Role"},        {"Esc", "Cancel"}};
            }
            else if (state_->viewing_only)
            {
                hints = AddExtraKeysThatFit({{"F7", "Export"}, {"Esc", "Leave"}},
                                            {{"F6", "Stn History"},
                                             {"F8", "Regulars"},
                                             {"F9", "Stn Card"},
                                             {"F10", "Summary"},
                                             {"F12", "Notes"}},
                                            1);
            }
            else
            {
                // The seldom-used keys (see InfoWindow) join the bar when
                // there's room.
                hints = AddExtraKeysThatFit(
                    {{"F2", "Check In"}, {"F3", "Edit"}, {"F4", "Close/Save"}, {"F5", "Delete"}, {"F7", "Export"}},
                    {{"F6", "Stn History"},
                     {"F8", "Regulars"},
                     {"F9", "Stn Card"},
                     {"F10", "Summary"},
                     {"F12", "Notes"}},
                    1);
            }
            // The check-in count, in the top bar.
            std::size_t count = state_->active_check_ins.size();
            return PageChrome(page_title, content, hints,
                              std::to_string(count) + (count == 1 ? " check-in" : " check-ins"));
        }

    private:
        // The date the net was started, with the time of day next to it:
        // remade only for another session (or the 12/24-hour setting), not
        // on every frame. A session's date and start never change.
        const std::string& StartedText() const
        {
            const NetInstance& instance = state_->active_instance;
            if (instance.id != started_id_ || instance.started_at != started_at_ ||
                Use24HourClock() != started_24_hour_)
            {
                started_id_ = instance.id;
                started_at_ = instance.started_at;
                started_24_hour_ = Use24HourClock();
                started_ = instance.instance_date;
                std::string start_time = FormatLocalTimeOfDay(instance.started_at);
                if (!start_time.empty())
                {
                    started_.append("  ");
                    started_.append(start_time);
                }
            }
            return started_;
        }

        AppState* state_;
        ftxui::Component check_in_menu_;
        mutable std::int64_t started_id_ = -1;
        mutable std::int64_t started_at_ = -1;
        mutable bool started_24_hour_ = false;
        mutable std::string started_;
    };

    // The keys in the check-in windows that jump to the fields usually
    // wanted after the callsign, past the station's details.
    static const std::vector<KeyHint> kJumpKeyHints = {{"F4", "Remarks"}, {"F5", "Comment"}, {"F6", "Role"}};

    // A check-in window's keys: `actions` (ending with Esc) and the jump
    // keys, on one row when the window is wide enough for them all, with
    // Esc last; otherwise the jump keys on a second row.
    static void AppendCheckInKeyRows(const AppState* state, std::vector<KeyHint> actions, ftxui::Elements* rows)
    {
        std::vector<KeyHint> all(actions.begin(), actions.end() - 1);
        all.insert(all.end(), kJumpKeyHints.begin(), kJumpKeyHints.end());
        all.push_back(actions.back());
        // Less the window's border.
        if (KeyHintRowWidth(all) <= CheckInWindowWidth(state->list_width) - 2)
        {
            rows->push_back(KeyHintRow(all));
            return;
        }
        rows->push_back(KeyHintRow(actions));
        rows->push_back(KeyHintRow(kJumpKeyHints));
    }

    // A check-in (or Saved Station) window's frame, as wide as the terminal
    // allows (see CheckInWindowWidth) rather than only as wide as what's
    // typed in it.
    static ftxui::Element CheckInWindow(const AppState* state, ftxui::Element content)
    {
        return std::move(content) | ftxui::color(kColorHeading) | ftxui::borderStyled(kColorDialogBorder) |
               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, CheckInWindowWidth(state->list_width));
    }

    // The New Station modal, shown on top of the active-net page.
    class NewStationModalRenderer
    {
    public:
        NewStationModalRenderer(AppState* state, StationFieldInputs inputs, ftxui::Component input_signal_report,
                                ftxui::Component input_remarks, ftxui::Component input_comment,
                                ftxui::Component role_choice_menu)
            : state_(state),
              inputs_(std::move(inputs)),
              input_signal_report_(std::move(input_signal_report)),
              input_remarks_(std::move(input_remarks)),
              input_comment_(std::move(input_comment)),
              role_choice_menu_(std::move(role_choice_menu))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(Heading("Log Station Check-In"));
            rows.push_back(DialogSeparator());
            ftxui::Elements field_rows = StationFieldRows(inputs_);
            rows.push_back(field_rows[0]);  // Callsign
            // While choosing a match, the list takes the place of the fields
            // below Callsign (which can't be typed in meanwhile), so it fits
            // an 80x24 screen; they're back once a match is picked, the
            // callsign is cleared or the cursor leaves the field.
            if (!state_->modal_callsign_suggestions.empty() && inputs_.callsign->Focused())
            {
                rows.push_back(MatchList(MatchListHeader(state_->list_width), state_->modal_callsign_suggestion_labels,
                                         state_->selected_suggestion_index));
            }
            else
            {
                ftxui::Elements fields(field_rows.begin() + 1, field_rows.end());
                fields.push_back(ftxui::hbox({FieldLabel("Signal Report: "), input_signal_report_->Render()}));
                fields.push_back(ftxui::hbox({FieldLabel("Remarks:       "), input_remarks_->Render()}));
                fields.push_back(ftxui::hbox({FieldLabel("Comment:       "), input_comment_->Render()}));
                // Last, in Tab order too: at the bottom of the right column
                // when the form is in two.
                fields.push_back(ftxui::vbox({FieldLabel("Additional Role (optional):"), role_choice_menu_->Render()}));
                AppendFormFields(state_, fields, &rows);
            }
            rows.push_back(DialogSeparator());
            AppendCheckInKeyRows(state_, {{"F2", "Log & Next"}, {"F3", "Log & Close"}, {"Esc", "Cancel"}}, &rows);
            rows.push_back(ErrorLine(state_->form_error));

            return CheckInWindow(state_, ftxui::vbox(std::move(rows)));
        }

    private:
        AppState* state_;
        StationFieldInputs inputs_;
        ftxui::Component input_signal_report_;
        ftxui::Component input_remarks_;
        ftxui::Component input_comment_;
        ftxui::Component role_choice_menu_;
    };

    // The Edit Check-in modal, shown on top of the active-net page. Callsign
    // isn't editable (see AppState::edit_checkin_original), so it's a plain
    // label rather than an Input (StationFieldInputs.callsign is left null).
    class EditCheckInModalRenderer
    {
    public:
        EditCheckInModalRenderer(AppState* state, StationFieldInputs inputs, ftxui::Component input_signal_report,
                                 ftxui::Component input_remarks, ftxui::Component input_comment,
                                 ftxui::Component role_choice_menu)
            : state_(state),
              inputs_(std::move(inputs)),
              input_signal_report_(std::move(input_signal_report)),
              input_remarks_(std::move(input_remarks)),
              input_comment_(std::move(input_comment)),
              role_choice_menu_(std::move(role_choice_menu))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(Heading("Edit Check-In"));
            rows.push_back(DialogSeparator());
            rows.push_back(
                ftxui::hbox({FieldLabel("Callsign:      "), ftxui::text(state_->edit_checkin_original.callsign)}));
            ftxui::Elements fields = StationFieldRows(inputs_);
            fields.push_back(ftxui::hbox({FieldLabel("Signal Report: "), input_signal_report_->Render()}));
            fields.push_back(ftxui::hbox({FieldLabel("Remarks:       "), input_remarks_->Render()}));
            fields.push_back(ftxui::hbox({FieldLabel("Comment:       "), input_comment_->Render()}));
            fields.push_back(ftxui::vbox({FieldLabel("Additional Role (optional):"), role_choice_menu_->Render()}));
            AppendFormFields(state_, fields, &rows);
            rows.push_back(DialogSeparator());
            AppendCheckInKeyRows(state_, {{"F2", "Save"}, {"Esc", "Cancel"}}, &rows);
            rows.push_back(ErrorLine(state_->form_error));

            return CheckInWindow(state_, ftxui::vbox(std::move(rows)));
        }

    private:
        AppState* state_;
        StationFieldInputs inputs_;
        ftxui::Component input_signal_report_;
        ftxui::Component input_remarks_;
        ftxui::Component input_comment_;
        ftxui::Component role_choice_menu_;
    };

    ftxui::Component BuildActiveNetPage(AppState* state)
    {
        ftxui::MenuOption check_in_menu_option;
        check_in_menu_option.focused_entry = &state->selected_check_in_index;
        check_in_menu_option.on_enter = EditSelectedCheckInHandler(state);
        check_in_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component check_in_menu = ClickableList(
            state, ftxui::Menu(&state->active_display_rows, &state->selected_check_in_index, check_in_menu_option));

        ftxui::Component main_root = ftxui::Container::Vertical({check_in_menu});
        ftxui::Component main_view = ftxui::Renderer(main_root, ActiveNetRenderer(state, check_in_menu));

        ftxui::InputOption callsign_option = SingleLineInputOption();
        callsign_option.on_enter = CallsignLookupHandler(state);
        callsign_option.on_change = CallsignSuggestHandler(state);
        ftxui::Component input_callsign =
            ftxui::Input(&state->modal_station.callsign, "Callsign (? = unsure)", callsign_option);
        StationFieldInputs modal_inputs = BuildStationFieldInputs(&state->modal_station, input_callsign);

        ftxui::Component input_signal_report =
            ftxui::Input(&state->modal_signal_report, "Signal Report", SingleLineInputOption());
        ftxui::InputOption remarks_option = SingleLineInputOption();
        remarks_option.cursor_position = &state->modal_remarks_cursor;
        ftxui::Component input_remarks = ftxui::Input(&state->modal_remarks, "Remarks", remarks_option);
        ftxui::InputOption comment_option = SingleLineInputOption();
        comment_option.cursor_position = &state->modal_comment_cursor;
        ftxui::Component input_comment = ftxui::Input(&state->modal_comment, "Comment", comment_option);
        // A Menu (not Radiobox) so arrow keys change the choice immediately --
        // no separate "confirm with Space/Enter" step, and no internal hover
        // cursor left dangling from a previous check-in. A Radiobox retains
        // its own hovered-entry state across the component's whole lifetime
        // (it's built once, like every other component in this app), so if
        // this modal is reopened for a second check-in after resetting
        // modal_role_choice_index to 0, the *hover* wouldn't reset with it --
        // the next arrow-down would move relative to wherever it was left
        // after the previous check-in, silently landing on the wrong role.
        ftxui::MenuOption role_choice_menu_option;
        role_choice_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component role_choice_menu =
            ftxui::Menu(&state->modal_role_choice_labels, &state->modal_role_choice_index, role_choice_menu_option);

        state->modal_callsign_input = input_callsign;
        state->modal_remarks_input = input_remarks;
        state->modal_comment_input = input_comment;
        state->modal_role_input = role_choice_menu;

        ftxui::Components modal_components = StationFieldComponents(modal_inputs);
        modal_components.push_back(input_signal_report);
        modal_components.push_back(input_remarks);
        modal_components.push_back(input_comment);
        modal_components.push_back(role_choice_menu);
        ftxui::Component modal_root = ftxui::Container::Vertical(modal_components);
        ftxui::Component modal_view =
            ftxui::Renderer(modal_root, NewStationModalRenderer(state, modal_inputs, input_signal_report, input_remarks,
                                                                input_comment, role_choice_menu));

        StationFieldInputs edit_checkin_inputs =
            BuildStationFieldInputs(&state->edit_checkin_station, ftxui::Component());
        ftxui::Component edit_input_signal_report =
            ftxui::Input(&state->edit_checkin_signal_report, "Signal Report", SingleLineInputOption());
        ftxui::InputOption edit_remarks_option = SingleLineInputOption();
        edit_remarks_option.cursor_position = &state->edit_checkin_remarks_cursor;
        ftxui::Component edit_input_remarks =
            ftxui::Input(&state->edit_checkin_remarks, "Remarks", edit_remarks_option);
        ftxui::InputOption edit_comment_option = SingleLineInputOption();
        edit_comment_option.cursor_position = &state->edit_checkin_comment_cursor;
        ftxui::Component edit_input_comment =
            ftxui::Input(&state->edit_checkin_comment, "Comment", edit_comment_option);
        ftxui::MenuOption edit_role_choice_menu_option;
        edit_role_choice_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component edit_role_choice_menu =
            ftxui::Menu(&state->edit_checkin_role_choice_labels, &state->edit_checkin_role_choice_index,
                        edit_role_choice_menu_option);

        state->edit_checkin_remarks_input = edit_input_remarks;
        state->edit_checkin_comment_input = edit_input_comment;
        state->edit_checkin_role_input = edit_role_choice_menu;

        ftxui::Components edit_modal_components = StationFieldComponents(edit_checkin_inputs);
        edit_modal_components.push_back(edit_input_signal_report);
        edit_modal_components.push_back(edit_input_remarks);
        edit_modal_components.push_back(edit_input_comment);
        edit_modal_components.push_back(edit_role_choice_menu);
        ftxui::Component edit_modal_root = ftxui::Container::Vertical(edit_modal_components);
        ftxui::Component edit_modal_view = ftxui::Renderer(
            edit_modal_root, EditCheckInModalRenderer(state, edit_checkin_inputs, edit_input_signal_report,
                                                      edit_input_remarks, edit_input_comment, edit_role_choice_menu));

        ftxui::Component with_new_station_modal = LayeredModal(main_view, modal_view, &state->show_new_station_modal);
        ftxui::Component with_edit_checkin_modal =
            LayeredModal(with_new_station_modal, edit_modal_view, &state->show_edit_checkin_modal);
        ftxui::Component with_notes_modal =
            LayeredModal(with_edit_checkin_modal, BuildSessionNotesModal(state), &state->show_session_notes_modal);
        return WithRowDeleteConfirm(
            state, LayeredModal(with_notes_modal, BuildZmodemConfirmModal(state), &state->show_zmodem_confirm_modal));
    }

    // ---- Settings page ---------------------------------------------------

    class SettingsRenderer
    {
    public:
        SettingsRenderer(AppState* state, ftxui::Component input_callsign, ftxui::Component input_gmrs,
                         ftxui::Component input_location, ftxui::Component input_radius, ftxui::Component input_server,
                         ftxui::Component time_format_toggle, ftxui::Component update_check_toggle)
            : state_(state),
              input_callsign_(std::move(input_callsign)),
              input_gmrs_(std::move(input_gmrs)),
              input_location_(std::move(input_location)),
              input_radius_(std::move(input_radius)),
              input_server_(std::move(input_server)),
              time_format_toggle_(std::move(time_format_toggle)),
              update_check_toggle_(std::move(update_check_toggle))
        {
        }

        ftxui::Element operator()() const
        {
            // At the console: Update Check, and the newer version if one's
            // been found.
            ftxui::Element update_check_row = ftxui::text("");
            ftxui::Element update_hint = ftxui::text("");
            if (state_->is_console_session)
            {
                update_check_row = ftxui::hbox({FieldLabel("Update Check:  "), update_check_toggle_->Render()});
                const std::string& available = AvailableUpdateForDisplay();
                update_hint = ftxui::vbox(
                    {ftxui::text(""),
                     HintParagraph(available.empty()
                                       ? "Update Check notifies you about new releases. It checks GitHub "
                                         "every 6 hours."
                                       : "QuickLogger " + available +
                                             " is out. Click the notice in the top bar to open its download "
                                             "page: " +
                                             std::string(kReleasesPageUrl))});
            }
            // The station data's status: one paragraph per line of it.
            const std::vector<std::string>& status_text = state_->station_status_lines;
            ftxui::Elements status_lines;
            status_lines.reserve(status_text.size());
            for (const std::string& status_line : status_text)
            {
                status_lines.push_back(ftxui::paragraph(status_line));
            }
            ftxui::Element station_status = ftxui::vbox(std::move(status_lines)) | ftxui::color(kColorHeading);
            // Two columns, even at 80 columns, to leave room below: Tab
            // goes across each row (see BuildSettingsPage). Time Format's
            // choices are too wide for a column, so it has a row of its own.
            // Your call signs: the console's own to set; an SSH user's set in
            // Manage Users.
            ftxui::Element amateur_row;
            ftxui::Element gmrs_row;
            if (state_->callsign_editable)
            {
                amateur_row = ftxui::hbox({FieldLabel("Amateur Call*: "), input_callsign_->Render()});
                gmrs_row = ftxui::hbox({FieldLabel("GMRS Call*:    "), input_gmrs_->Render()});
            }
            else
            {
                amateur_row = ftxui::hbox({FieldLabel("Amateur Call:  "), LockedCallsign(state_->settings.callsign)});
                gmrs_row = ftxui::hbox({FieldLabel("GMRS Call:     "), LockedCallsign(state_->settings.gmrs_callsign)});
            }
            ftxui::Element left_column = ftxui::vbox({
                amateur_row,
                gmrs_row,
                ftxui::hbox({FieldLabel("Nearby Radius: "),
                             input_radius_->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 4),
                             ftxui::text("miles")}),
            });
            ftxui::Element right_column = ftxui::vbox({
                ftxui::hbox({FieldLabel("Postal Code*:  "), input_location_->Render()}),
                update_check_row,
                state_->is_console_session ? ftxui::hbox({FieldLabel("Server:        "), input_server_->Render()})
                                           : ftxui::text(""),
            });
            // The Server field is the console's alone.
            ftxui::Element server_hint = ftxui::text("");
            if (state_->is_console_session)
            {
                server_hint = ftxui::vbox(
                    {ftxui::text(""),
                     HintParagraph("Server (host or host:port) is shown in SSH users' sftp and scp commands.")});
            }
            ftxui::Element content = ftxui::vbox({
                ftxui::hbox({left_column | ftxui::xflex, ftxui::text("   "), right_column | ftxui::xflex}),
                ftxui::hbox({FieldLabel("Time Format:   "), time_format_toggle_->Render()}),
                HintText(state_->callsign_editable ? "* Required: the postal code, and one call sign or both"
                                                   : "* Required. Your call signs are set by the server's operator."),
                Separator(),
                HintParagraph("Postal Code (5-digit ZIP, or Canadian postal code) finds nearby licensed stations "
                              "for nets without one."),
                ftxui::text(""),
                HintParagraph(
                    "Nearby Radius (1-250 miles, 70 if blank) is how far from the net's postal code, or yours, a "
                    "suggested station can be."),
                ftxui::text(""),
                HintParagraph("Time Format (Left/Right to change) sets how times are shown and exported. Times use "
                              "the time zone of the computer QuickLogger runs on."),
                server_hint,
                update_hint,
                Separator(),
                Heading("Station data (shared globally, kept up to date automatically):"),
                station_status,
                StatusLine(state_->status_message),
                ErrorLine(state_->form_error),
            });

            std::vector<KeyHint> hints = {{"F2", "Save"}};
            if (state_->is_console_session)
            {
                hints.push_back({"F3", "Refresh Data"});
            }
            // One meaning per key, whoever is logged in: F4 your own keys,
            // F5 other people (as far as your level reaches), F6 the
            // console's upstream.
            if (CanEditOwnKeys(state_))
            {
                hints.push_back({"F4", "My Keys"});
            }
            if (CanManageUsers(state_))
            {
                hints.push_back({"F5", "Manage Users"});
            }
            else if (!state_->is_console_session && state_->access.level == kAccessNetAdmin)
            {
                hints.push_back({"F5", "Net Access"});
            }
            if (state_->is_console_session)
            {
                hints.push_back({"F6", "Upstream"});
            }
            hints.push_back({"Esc", "Cancel"});
            return PageChrome("Settings", content, hints);
        }

    private:
        // An SSH user's call sign, or "(none)".
        static ftxui::Element LockedCallsign(const std::string& callsign)
        {
            return callsign.empty() ? HintText("(none)") : ftxui::text(callsign) | ftxui::color(kColorData);
        }

        AppState* state_;
        ftxui::Component input_callsign_;
        ftxui::Component input_gmrs_;
        ftxui::Component input_location_;
        ftxui::Component input_radius_;
        ftxui::Component input_server_;
        ftxui::Component time_format_toggle_;
        ftxui::Component update_check_toggle_;
    };

    // The Upstream Server window over Settings (F6, console only).
    class UpstreamWindowRenderer
    {
    public:
        UpstreamWindowRenderer(AppState* state, ftxui::Component input_host, ftxui::Component input_user,
                               ftxui::Component input_port)
            : state_(state),
              input_host_(std::move(input_host)),
              input_user_(std::move(input_user)),
              input_port_(std::move(input_port))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(Heading("Upstream Server"));
            rows.push_back(DialogSeparator());
            rows.push_back(ftxui::hbox({FieldLabel("Host:      "), input_host_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Username:  "), input_user_->Render()}));
            rows.push_back(ftxui::hbox(
                {FieldLabel("Port:      "), input_port_->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 6)}));
            rows.push_back(DialogSeparator());
            rows.push_back(
                HintParagraph("Push sessions and nets here from an Export window (F3), or when closing a "
                              "net (F3). Pull them with F4 on an Import page. A blank host means none."));
            rows.push_back(ftxui::text(""));
            rows.push_back(
                HintParagraph("Pushing and pulling use this computer's ssh and your own ssh key. Log in there "
                              "once with ssh first. A key with a passphrase must be in ssh-agent."));
            if (!state_->upstream_tools_available)
            {
                rows.push_back(ftxui::text(""));
                rows.push_back(ErrorLine(kNoSshMessage));
            }
            rows.push_back(DialogSeparator());
            rows.push_back(KeyHintRow({{"F2", "Save"}, {"Esc", "Cancel"}}));
            rows.push_back(ErrorLine(state_->form_error));
            return CheckInWindow(state_, ftxui::vbox(std::move(rows)));
        }

    private:
        AppState* state_;
        ftxui::Component input_host_;
        ftxui::Component input_user_;
        ftxui::Component input_port_;
    };

    // The My Keys window over an SSH user's Settings (F4): their keys and
    // the comment of the highlighted one.
    class MyKeysWindowRenderer
    {
    public:
        MyKeysWindowRenderer(AppState* state, ftxui::Component key_menu, ftxui::Component input_comment,
                             ftxui::Component transfer_toggle, ftxui::Component input_new_key)
            : state_(state),
              key_menu_(std::move(key_menu)),
              input_comment_(std::move(input_comment)),
              transfer_toggle_(std::move(transfer_toggle)),
              input_new_key_(std::move(input_new_key))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(Heading("My Keys"));
            rows.push_back(DialogSeparator());
            rows.push_back(Framed(ftxui::vbox({
                ColumnHeader(MyKeyListHeader(state_->list_width)),
                key_menu_->Render() | ftxui::yframe | ftxui::vscroll_indicator |
                    ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 8),
            })));
            rows.push_back(ftxui::hbox({FieldLabel("Comment:  "), input_comment_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Transfer: "), transfer_toggle_->Render()}));
            if (state_->my_keys_self_service)
            {
                rows.push_back(ftxui::hbox({FieldLabel("Add key:  "), input_new_key_->Render()}));
            }
            rows.push_back(DialogSeparator());
            rows.push_back(
                HintParagraph("Up/Down picks a key, Tab moves between fields. Transfer: ZMODEM waits for your "
                              "terminal to receive, SFTP shows scp commands, Ask tries ZMODEM and remembers "
                              "what worked."));
            rows.push_back(DialogSeparator());
            if (state_->my_keys_self_service)
            {
                rows.push_back(KeyHintRow({{"F2", "Save"}, {"F3", "Remove Key"}, {"F4", "Add Key"}, {"Esc", "Close"}}));
            }
            else
            {
                rows.push_back(KeyHintRow({{"F2", "Save"}, {"Esc", "Close"}}));
            }
            rows.push_back(StatusLine(state_->status_message));
            rows.push_back(ErrorLine(state_->form_error));
            return CheckInWindow(state_, ftxui::vbox(std::move(rows)));
        }

    private:
        AppState* state_;
        ftxui::Component key_menu_;
        ftxui::Component input_comment_;
        ftxui::Component transfer_toggle_;
        ftxui::Component input_new_key_;
    };

    static ftxui::Component BuildMyKeysWindow(AppState* state)
    {
        ftxui::MenuOption key_menu_option;
        key_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        key_menu_option.on_change = MyKeySelectionHandler(state);
        // One position for the highlight and the selection: left separate, the
        // drawn cursor starts on the first key whichever one is selected.
        key_menu_option.focused_entry = &state->selected_my_key_index;
        // Tab leaves the list for the comment and Transfer fields (a Menu
        // would take it as "next key").
        ftxui::Component key_menu = std::make_shared<IgnoreTab>(
            ClickableList(state, ftxui::Menu(&state->my_keys_labels, &state->selected_my_key_index, key_menu_option)));
        ftxui::Component input_comment =
            ftxui::Input(&state->my_key_comment_text, "e.g. My Mac", SingleLineInputOption());
        ftxui::MenuOption transfer_option = ftxui::MenuOption::Toggle();
        transfer_option.entries_option.transform = ToggleEntryTransform;
        transfer_option.elements_infix = ToggleGap;
        transfer_option.focused_entry = &state->my_key_transfer_index;
        ftxui::Component transfer_toggle = std::make_shared<IgnoreTab>(
            ftxui::Menu(&state->my_key_transfer_labels, &state->my_key_transfer_index, transfer_option));
        // Only while the admin allows it (see AppState::my_keys_self_service).
        ftxui::Component input_new_key =
            ftxui::Maybe(ftxui::Input(&state->my_key_new_text, "ssh-ed25519 AAAA... comment",
                                      PublicKeyInputOption(&state->my_key_new_text)),
                         &state->my_keys_self_service);
        return ftxui::Renderer(ftxui::Container::Vertical({key_menu, input_comment, transfer_toggle, input_new_key},
                                                          &state->my_keys_focus),
                               MyKeysWindowRenderer(state, key_menu, input_comment, transfer_toggle, input_new_key));
    }

    static ftxui::Component BuildUpstreamWindow(AppState* state)
    {
        ftxui::InputOption host_option = SingleLineInputOption();
        host_option.cursor_position = &state->upstream_host_cursor;
        ftxui::Component input_host = ftxui::Input(&state->upstream_host_text, "upstream.example.org", host_option);
        ftxui::InputOption user_option = SingleLineInputOption();
        user_option.cursor_position = &state->upstream_user_cursor;
        ftxui::Component input_user = ftxui::Input(&state->upstream_user_text, "Your username there", user_option);
        ftxui::InputOption port_option = SingleLineInputOption();
        port_option.cursor_position = &state->upstream_port_cursor;
        port_option.on_change = DigitsFieldHandler(&state->upstream_port_text, 5);
        ftxui::Component input_port = ftxui::Input(&state->upstream_port_text, "22", port_option);
        return ftxui::Renderer(ftxui::Container::Vertical({input_host, input_user, input_port}, &state->upstream_focus),
                               UpstreamWindowRenderer(state, input_host, input_user, input_port));
    }

    ftxui::Component BuildSettingsPage(AppState* state)
    {
        ftxui::InputOption callsign_option = SingleLineInputOption();
        callsign_option.on_change = UppercaseFieldHandler(&state->settings_form.callsign);
        // Can't be focused or typed into while the callsign is the SSH
        // user's username (see AppState::callsign_editable).
        ftxui::Component input_callsign = ftxui::Maybe(
            ftxui::Input(&state->settings_form.callsign, "e.g. W4KWK", callsign_option), &state->callsign_editable);
        ftxui::InputOption gmrs_option = SingleLineInputOption();
        gmrs_option.on_change = UppercaseFieldHandler(&state->settings_form.gmrs_callsign);
        ftxui::Component input_gmrs = ftxui::Maybe(
            ftxui::Input(&state->settings_form.gmrs_callsign, "e.g. WSIP663", gmrs_option), &state->callsign_editable);
        ftxui::InputOption location_option = SingleLineInputOption();
        location_option.on_change = ZipCodeFieldHandler(&state->settings_form.location);
        ftxui::Component input_location =
            ftxui::Input(&state->settings_form.location, "ZIP or postal code", location_option);
        ftxui::InputOption radius_option = SingleLineInputOption();
        radius_option.on_change = DigitsFieldHandler(&state->settings_radius_text, 3);
        ftxui::Component input_radius = ftxui::Input(&state->settings_radius_text, "70", radius_option);
        ftxui::Component input_server = ftxui::Maybe(
            ftxui::Input(&state->settings_server_text, &state->settings_server_placeholder, SingleLineInputOption()),
            &state->is_console_session);

        ftxui::MenuOption time_format_option = ftxui::MenuOption::Toggle();
        time_format_option.entries_option.transform = ToggleEntryTransform;
        time_format_option.elements_infix = ToggleGap;
        time_format_option.focused_entry = &state->settings_time_format_index;
        ftxui::Component time_format_toggle = std::make_shared<IgnoreTab>(
            ftxui::Menu(&state->settings_time_format_labels, &state->settings_time_format_index, time_format_option));

        // Only at the console: an SSH user can't update the server.
        ftxui::MenuOption update_check_option = ftxui::MenuOption::Toggle();
        update_check_option.entries_option.transform = ToggleEntryTransform;
        update_check_option.elements_infix = ToggleGap;
        update_check_option.focused_entry = &state->settings_update_check_index;
        ftxui::Component update_check_toggle = ftxui::Maybe(
            std::make_shared<IgnoreTab>(ftxui::Menu(&state->settings_update_check_labels,
                                                    &state->settings_update_check_index, update_check_option)),
            &state->is_console_session);

        // In the order they're drawn: across each row of the two columns,
        // then Time Format below them.
        ftxui::Component root = ftxui::Container::Vertical(
            {
                input_callsign,
                input_location,
                input_gmrs,
                update_check_toggle,
                input_radius,
                input_server,
                time_format_toggle,
            },
            &state->settings_focus);

        ftxui::Component page =
            ftxui::Renderer(root, SettingsRenderer(state, input_callsign, input_gmrs, input_location, input_radius,
                                                   input_server, time_format_toggle, update_check_toggle));
        return LayeredModal(LayeredModal(page, BuildUpstreamWindow(state), &state->show_upstream_window),
                            BuildMyKeysWindow(state), &state->show_my_keys_window);
    }

    // ---- Ad hoc net page ---------------------------------------------------

    class AdHocNetRenderer
    {
    public:
        AdHocNetRenderer(AppState* state, ftxui::Component input_name, const NewNetRadioInputs& radio,
                         ftxui::Component input_location, ftxui::Component open_session_menu)
            : state_(state),
              input_name_(std::move(input_name)),
              service_(radio.service),
              input_mode_(radio.mode),
              input_frequency_(radio.frequency),
              input_offset_(radio.offset),
              channel_(radio.channel),
              input_tone_(radio.tone),
              input_location_(std::move(input_location)),
              partial_match_(radio.partial_match),
              open_session_menu_(std::move(open_session_menu))
        {
        }

        ftxui::Element operator()() const
        {
            bool view_only = state_->view_only_user;
            ftxui::Elements rows;
            std::vector<KeyHint> hints;
            if (view_only)
            {
                // No new ad hoc net for a view-only user: just the open
                // ones to view, and the history.
                rows.push_back(
                    HintParagraph("One-off nets. Ad hoc nets aren't listed with the "
                                  "recurring nets; F6 shows their history."));
                if (state_->open_ad_hoc_sessions.empty())
                {
                    rows.push_back(HintText("No ad hoc session is open."));
                }
            }
            else
            {
                rows = {
                    HintParagraph("Logs an ad hoc (non-recurring) net; F6 shows history."),
                    Separator(),
                    ftxui::hbox({FieldLabel("Name:             "), input_name_->Render()}),
                    ftxui::hbox({FieldLabel("Service:          "), service_->Render()}),
                };
                AppendNewNetRadioRows(state_->new_net_gmrs, input_mode_, input_frequency_, input_offset_, channel_,
                                      input_tone_, &rows);
                rows.push_back(ftxui::hbox({FieldLabel("Postal Code:      "), input_location_->Render()}));
                if (!state_->new_net_gmrs)
                {
                    rows.push_back(PartialMatchRow("Partial Matching: ", partial_match_));
                }
                hints.push_back({"F2", "Log Net"});
            }
            if (!state_->open_ad_hoc_sessions.empty())
            {
                rows.push_back(Separator());
                rows.push_back(Heading(view_only ? "Open (F3 views one):" : "Still open (F3 resumes one):"));
                rows.push_back(Framed(OpenSessionRows()));
                rows.push_back(PickPrompt(state_, PickList::kOpenAdHocSessions));
                hints.push_back({"F3", view_only ? "View" : "Resume"});
            }
            rows.push_back(StatusLine(state_->status_message));
            rows.push_back(ErrorLine(state_->form_error));
            hints.push_back({"F6", "History"});
            hints.push_back({"Esc", view_only ? "Back" : "Cancel"});

            if (state_->row_pick_action != RowPickAction::kNone)
            {
                return PageChrome("Ad Hoc Net", ftxui::vbox(std::move(rows)), PickKeyHints(state_));
            }
            return PageChrome("Ad Hoc Net", ftxui::vbox(std::move(rows)), hints);
        }

    private:
        // The open sessions: numbered while picking one to resume; otherwise
        // plain rows, since the list isn't something to move around in.
        ftxui::Element OpenSessionRows() const
        {
            if (IsPicking(state_, PickList::kOpenAdHocSessions))
            {
                return PickableRows(state_, PickList::kOpenAdHocSessions, state_->open_ad_hoc_labels,
                                    state_->selected_open_ad_hoc_index, open_session_menu_);
            }
            ftxui::Elements lines;
            for (const std::string& label : state_->open_ad_hoc_labels)
            {
                lines.push_back(ftxui::text("  " + label) | ftxui::color(kColorListRow));
            }
            return ftxui::vbox(std::move(lines));
        }

        AppState* state_;
        ftxui::Component input_name_;
        ftxui::Component service_;
        ftxui::Component input_mode_;
        ftxui::Component input_frequency_;
        ftxui::Component input_offset_;
        ftxui::Component channel_;
        ftxui::Component input_tone_;
        ftxui::Component input_location_;
        ftxui::Component partial_match_;
        ftxui::Component open_session_menu_;
    };

    ftxui::Component BuildAdHocNetPage(AppState* state)
    {
        ftxui::Component input_name = ftxui::Input(&state->new_net_name, "e.g. Tailgate Net", SingleLineInputOption());
        NewNetRadioInputs radio = BuildNewNetRadioInputs(state);
        ftxui::InputOption location_option = SingleLineInputOption();
        location_option.on_change = ZipCodeFieldHandler(&state->new_net_location);
        ftxui::Component input_location =
            ftxui::Input(&state->new_net_location, "ZIP/postal (optional)", location_option);

        ftxui::Component root = ftxui::Container::Vertical({
            input_name,
            radio.service,
            radio.mode,
            radio.frequency,
            radio.offset,
            radio.channel,
            radio.tone,
            input_location,
            radio.partial_match,
        });

        state->ad_hoc_net_name_input = input_name;

        // Only rendered while picking (see AdHocNetRenderer::OpenSessionRows),
        // and deliberately left out of `root`: Tab and the arrow keys stay on
        // the form's fields.
        // Highlight bound to the selection, as on the net list.
        ftxui::MenuOption open_session_menu_option;
        open_session_menu_option.focused_entry = &state->selected_open_ad_hoc_index;
        ftxui::Component open_session_menu = ClickableList(
            state,
            ftxui::Menu(&state->open_ad_hoc_labels, &state->selected_open_ad_hoc_index, open_session_menu_option));

        return ftxui::Renderer(root, AdHocNetRenderer(state, input_name, radio, input_location, open_session_menu));
    }

    // ---- Net history page ---------------------------------------------------

    // An F-key's number (F9 is 9, F10 is 10); anything else sorts after
    // every F-key.
    static int FunctionKeyNumber(const KeyHint& hint)
    {
        if (hint.key.size() < 2 || hint.key[0] != 'F' || hint.key[1] < '0' || hint.key[1] > '9')
        {
            return 1000;
        }
        return std::atoi(hint.key.c_str() + 1);
    }

    static bool ComesBeforeInKeyOrder(const KeyHint& first, const KeyHint& second)
    {
        return FunctionKeyNumber(first) < FunctionKeyNumber(second);
    }

    // `hints` with the F-keys in number order (Esc and the like stay after
    // them, in the order given) -- for a bar whose extra keys (see
    // AddExtraKeysThatFit) fall between its others.
    static std::vector<KeyHint> InKeyOrder(std::vector<KeyHint> hints)
    {
        std::stable_sort(hints.begin(), hints.end(), ComesBeforeInKeyOrder);
        return hints;
    }

    class NetHistoryRenderer
    {
    public:
        NetHistoryRenderer(AppState* state, ftxui::Component instance_menu, ftxui::Component checkin_menu)
            : state_(state), instance_menu_(std::move(instance_menu)), checkin_menu_(std::move(checkin_menu))
        {
        }

        ftxui::Element operator()() const
        {
            std::string net_name = "Ad Hoc Nets";
            if (!state_->history_ad_hoc && state_->selected_net_index < static_cast<int>(state_->nets.size()))
            {
                net_name = state_->nets[state_->selected_net_index].name;
            }

            ftxui::Element instance_list =
                state_->history_instances.empty()
                    ? HintText(state_->history_ad_hoc ? "No ad hoc nets yet." : "No past instances of this net yet.")
                    : PickableRows(state_, PickList::kNetInstances, state_->history_instance_labels,
                                   state_->selected_history_index, instance_menu_) |
                          ftxui::yframe | ftxui::vscroll_indicator;

            ftxui::Element checkin_list =
                state_->history_check_in_labels.empty()
                    ? HintText(state_->history_instances.empty() ? "" : "No check-ins were logged.")
                    : PickableRows(state_, PickList::kHistoryCheckIns, state_->history_check_in_labels,
                                   state_->selected_history_check_in_index, checkin_menu_) |
                          ftxui::yframe | ftxui::vscroll_indicator;

            ftxui::Element content = ftxui::vbox({
                Framed(ftxui::vbox({
                    PickableColumnHeader(state_, PickList::kNetInstances,
                                         NetInstanceListHeader(state_->list_width, state_->history_ad_hoc)),
                    instance_list,
                })) |
                    ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, SessionBoxHeight()),
                PickPrompt(state_, PickList::kNetInstances),
                Separator(),
                Framed(ftxui::vbox({
                    ColumnHeader(CheckInListHeader(state_->list_width)),
                    checkin_list,
                })) |
                    ftxui::flex,
                PickPrompt(state_, PickList::kHistoryCheckIns),
                StatusLine(state_->status_message),
                ErrorLine(state_->form_error),
            });

            if (state_->row_pick_action != RowPickAction::kNone)
            {
                return PageChrome("History: " + net_name, content, PickKeyHints(state_));
            }
            // At 80 columns the bar has room for F8 but not F5 or F9; they
            // always work and appear once there's room (Help lists them).
            std::vector<KeyHint> extras;
            if (!state_->history_ad_hoc)
            {
                extras.push_back({"F8", "Stats"});
            }
            // What this user may do with the net: delete (its Net Admins and
            // Admins), import (anyone it was given to), or only look.
            NetPower power = HistoryPower(state_);
            if (power == NetPower::kManage)
            {
                extras.push_back({"F4", "Del Session"});
            }
            extras.push_back({"F9", "Find Station"});
            extras.push_back({"F12", "Notes"});
            if (power == NetPower::kWatch)
            {
                return PageChrome("History: " + net_name, content,
                                  InKeyOrder(AddExtraKeysThatFit({{"F7", "Export"}, {"Esc", "Back"}}, extras, 1)));
            }
            std::vector<KeyHint> hints;
            if (power == NetPower::kManage)
            {
                hints.push_back({"F5", "Del Check-In"});
            }
            hints.push_back({"F6", "Import"});
            hints.push_back({"F7", "Export"});
            hints.push_back({"Esc", "Back"});
            return PageChrome("History: " + net_name, content, InKeyOrder(AddExtraKeysThatFit(hints, extras, 1)));
        }

    private:
        // The sessions box's fixed height (its border and header included),
        // with the check-ins box below taking the rest. Sharing the space
        // flexibly let a long check-in list squeeze the sessions down to
        // nothing on an 80x24 terminal. Up to a third of the screen, but at
        // least three rows -- or just enough for however many sessions there
        // are, if fewer.
        int SessionBoxHeight() const
        {
            int rows = static_cast<int>(state_->history_instances.size());
            int most = std::max(3, (FrameTerminalSize().dimy - 10) / 3);
            rows = std::max(1, std::min(rows, most));
            return rows + 3;
        }

        AppState* state_;
        ftxui::Component instance_menu_;
        ftxui::Component checkin_menu_;
    };

    ftxui::Component BuildNetHistoryPage(AppState* state)
    {
        ftxui::MenuOption instance_menu_option;
        instance_menu_option.focused_entry = &state->selected_history_index;
        instance_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        instance_menu_option.on_change = HistoryInstanceChangedHandler(state);
        ftxui::Component instance_menu = ClickableList(
            state, ftxui::Menu(&state->history_instance_labels, &state->selected_history_index, instance_menu_option));

        ftxui::MenuOption checkin_menu_option;
        checkin_menu_option.focused_entry = &state->selected_history_check_in_index;
        checkin_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component checkin_menu = ClickableList(
            state,
            ftxui::Menu(&state->history_check_in_labels, &state->selected_history_check_in_index, checkin_menu_option));

        ftxui::Component root = ftxui::Container::Vertical({instance_menu, checkin_menu});
        ftxui::Component main_view = ftxui::Renderer(root, NetHistoryRenderer(state, instance_menu, checkin_menu));
        ftxui::Component with_notes_modal =
            LayeredModal(main_view, BuildSessionNotesModal(state), &state->show_session_notes_modal);
        return WithRowDeleteConfirm(
            state, LayeredModal(with_notes_modal, BuildZmodemConfirmModal(state), &state->show_zmodem_confirm_modal));
    }

    // ---- Edit net page ---------------------------------------------------

    // Shown before Database::DeleteNetCompletely actually runs (F8 on the
    // edit-net page) -- this is the one delete in the whole app that erases
    // real history (every instance and check-in under the net, not just a
    // single row), so it has its own confirmation rather than the numbered
    // pick the single-row deletes use. Same bare-Renderer-over-
    // empty-Container shape as BuildZmodemConfirmModal, for the same reason:
    // no interactive fields of its own, F2/Enter/Esc are handled by
    // EditNetKeyHandler's guard.
    class DeleteNetConfirmModalRenderer
    {
    public:
        explicit DeleteNetConfirmModalRenderer(AppState* state) : state_(state) {}

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(ftxui::text("Delete Net") | ftxui::bold | ftxui::color(kColorDanger));
            rows.push_back(DialogSeparator());
            rows.push_back(ftxui::text("Permanently delete \"" + state_->edit_net_name + "\"?") |
                           ftxui::color(kColorLabel));
            rows.push_back(ftxui::text(""));
            rows.push_back(ftxui::text("This removes every occurrence of this net, its full"));
            rows.push_back(ftxui::text("check-in history, and its saved-station list. Cannot"));
            rows.push_back(ftxui::text("be undone."));
            rows.push_back(DialogSeparator());
            rows.push_back(KeyHintRow({{"F2/Enter", "Delete"}, {"Esc", "Cancel"}}));
            return ftxui::vbox(std::move(rows)) | ftxui::color(kColorDanger) | ftxui::borderStyled(kColorDialogBorder);
        }

    private:
        AppState* state_;
    };

    static ftxui::Component BuildDeleteNetConfirmModal(AppState* state)
    {
        ftxui::Component root = ftxui::Container::Vertical({});
        return ftxui::Renderer(root, DeleteNetConfirmModalRenderer(state));
    }

    class EditNetRenderer
    {
    public:
        EditNetRenderer(AppState* state, ftxui::Component input_name, ftxui::Component input_mode,
                        ftxui::Component input_frequency, ftxui::Component input_offset, ftxui::Component channel,
                        ftxui::Component input_tone, ftxui::Component input_location, ftxui::Component input_recurrence,
                        ftxui::Component input_comments, ftxui::Component partial_match,
                        ftxui::Component saved_station_menu)
            : state_(state),
              input_name_(std::move(input_name)),
              input_mode_(std::move(input_mode)),
              input_frequency_(std::move(input_frequency)),
              input_offset_(std::move(input_offset)),
              channel_(std::move(channel)),
              input_tone_(std::move(input_tone)),
              input_location_(std::move(input_location)),
              input_recurrence_(std::move(input_recurrence)),
              input_comments_(std::move(input_comments)),
              partial_match_(std::move(partial_match)),
              saved_station_menu_(std::move(saved_station_menu))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element saved_station_list =
                state_->edit_net_saved_stations.empty()
                    ? HintText("No saved stations yet. F6 adds one.")
                    : PickableRows(state_, PickList::kSavedStations, state_->edit_net_saved_station_labels,
                                   state_->selected_saved_station_index, saved_station_menu_) |
                          ftxui::yframe | ftxui::vscroll_indicator;

            // Name and Mode across the whole width, as Mode's choices don't
            // fit in half of it; the rest in two columns when there's room.
            ftxui::Elements rows{
                ftxui::hbox({FieldLabel("Name:             "), input_name_->Render()}),
            };
            ftxui::Elements fields;
            if (!state_->edit_net_editable)
            {
                // A net they may only log: its details, as text.
                rows = {
                    ftxui::hbox({FieldLabel("Name:             "),
                                 ftxui::text(state_->edit_net_name) | ftxui::color(kColorData)}),
                    ftxui::hbox({FieldLabel("Radio:            "),
                                 ftxui::text(state_->edit_net_radio_summary) | ftxui::color(kColorData)}),
                };
                fields = {
                    ftxui::hbox({FieldLabel("Postal Code:      "),
                                 ftxui::text(state_->edit_net_location) | ftxui::color(kColorData)}),
                    ftxui::hbox({FieldLabel("Recurrence:       "),
                                 ftxui::text(state_->edit_net_recurrence) | ftxui::color(kColorData)}),
                    ftxui::hbox({FieldLabel("Comments:         "),
                                 ftxui::text(state_->edit_net_comments) | ftxui::color(kColorData)}),
                };
            }
            else if (state_->edit_net_gmrs)
            {
                // A GMRS net: FM, on one of the channels, and the FCC's data.
                rows.push_back(ftxui::hbox({FieldLabel("Service:          "),
                                            ftxui::text("GMRS") | ftxui::color(kColorData), HintText("   Mode: FM")}));
                fields = {
                    ftxui::hbox({FieldLabel("Channel:          "), channel_->Render()}),
                    ftxui::hbox({FieldLabel("PL Tone:          "), input_tone_->Render()}),
                    ftxui::hbox({FieldLabel("Postal Code:      "), input_location_->Render()}),
                    ftxui::hbox({FieldLabel("Recurrence:       "), input_recurrence_->Render()}),
                    ftxui::hbox({FieldLabel("Comments:         "), input_comments_->Render()}),
                };
            }
            else
            {
                rows.push_back(ftxui::hbox({FieldLabel("Mode:             "), input_mode_->Render()}));
                if (state_->edit_net_mode_was_blank)
                {
                    rows.push_back(HintText("                  The old mode wasn't one of these. Pick one."));
                }
                fields = {
                    ftxui::hbox({FieldLabel("Frequency:        "), input_frequency_->Render()}),
                    ftxui::hbox({FieldLabel("Offset:           "), input_offset_->Render()}),
                    ftxui::hbox({FieldLabel("PL Tone:          "), input_tone_->Render()}),
                    ftxui::hbox({FieldLabel("Postal Code:      "), input_location_->Render()}),
                    ftxui::hbox({FieldLabel("Recurrence:       "), input_recurrence_->Render()}),
                    ftxui::hbox({FieldLabel("Comments:         "), input_comments_->Render()}),
                    // Last, in Tab order too: at the bottom of the right
                    // column when the form is in two.
                    PartialMatchRow("Partial Matching: ", partial_match_),
                };
            }
            AppendFormFields(state_, fields, &rows);
            if (!state_->edit_net_editable)
            {
                rows.push_back(HintText("Only a Net Admin changes these. Saved stations are yours to add and edit."));
            }
            rows.push_back(Separator());
            rows.push_back(Heading("Saved Stations:"));
            rows.push_back(
                Framed(ftxui::vbox({
                    PickableColumnHeader(state_, PickList::kSavedStations, SavedStationListHeader(state_->list_width)),
                    saved_station_list,
                })) |
                ftxui::flex);
            rows.push_back(PickPrompt(state_, PickList::kSavedStations));
            rows.push_back(StatusLine(state_->status_message));
            if (!state_->show_saved_station_modal)
            {
                rows.push_back(ErrorLine(state_->form_error));
            }

            if (state_->row_pick_action != RowPickAction::kNone)
            {
                return PageChrome("Edit Net: " + state_->edit_net_name, ftxui::vbox(std::move(rows)),
                                  PickKeyHints(state_));
            }
            if (state_->show_saved_station_modal)
            {
                return PageChrome("Edit Net: " + state_->edit_net_name, ftxui::vbox(std::move(rows)),
                                  {{"F2", "Save & Continue"}, {"F3", "Save & Close"}, {"Esc", "Cancel"}});
            }
            if (!state_->edit_net_editable)
            {
                return PageChrome("Edit Net: " + state_->edit_net_name, ftxui::vbox(std::move(rows)),
                                  AddExtraKeysThatFit(
                                      {
                                          {"F3", "Edit Station"},
                                          {"F6", "Add Station"},
                                          {"F7", "Export"},
                                          {"Esc", "Back"},
                                      },
                                      {{"F5", "Quiet Stations"}}, 2));
            }
            return PageChrome("Edit Net: " + state_->edit_net_name, ftxui::vbox(std::move(rows)),
                              AddExtraKeysThatFit(
                                  {
                                      {"F2", "Save & Close"},
                                      {"F3", "Edit Station"},
                                      {"F4", "Remove"},
                                      {"F6", "Add Station"},
                                      {"F7", "Export"},
                                      {"F8", "Del Net"},
                                      {"Esc", "Cancel"},
                                  },
                                  // This bar takes a while to fit on one
                                  // line; F5 may use a second one.
                                  {{"F5", "Quiet Stations"}}, 2));
        }

    private:
        AppState* state_;
        ftxui::Component input_name_;
        ftxui::Component input_mode_;
        ftxui::Component input_frequency_;
        ftxui::Component input_offset_;
        ftxui::Component channel_;
        ftxui::Component input_tone_;
        ftxui::Component input_location_;
        ftxui::Component input_recurrence_;
        ftxui::Component input_comments_;
        ftxui::Component partial_match_;
        ftxui::Component saved_station_menu_;
    };

    // The Saved Station window over the Edit Net page (F6 Add Station, F9
    // Edit Station): one station's details and its default remarks for this
    // net, with the same callsign autocomplete as the New Check-In window.
    class SavedStationModalRenderer
    {
    public:
        SavedStationModalRenderer(AppState* state, StationFieldInputs inputs, ftxui::Component remarks_input)
            : state_(state), inputs_(std::move(inputs)), remarks_input_(std::move(remarks_input))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(Heading("Saved Station"));
            rows.push_back(DialogSeparator());
            ftxui::Elements field_rows = StationFieldRows(inputs_);
            rows.push_back(field_rows[0]);  // Callsign
            // As in the New Check-In window, matches stand in for the other
            // fields while choosing.
            if (!state_->saved_station_suggestions.empty() && inputs_.callsign->Focused())
            {
                rows.push_back(MatchList(MatchListHeader(state_->list_width), state_->saved_station_suggestion_labels,
                                         state_->selected_saved_station_suggestion_index));
            }
            else
            {
                ftxui::Elements fields(field_rows.begin() + 1, field_rows.end());
                fields.push_back(ftxui::hbox({FieldLabel("Remarks:       "), remarks_input_->Render()}));
                AppendFormFields(state_, fields, &rows);
            }
            rows.push_back(DialogSeparator());
            rows.push_back(KeyHintRow({{"F2", "Save & Continue"}, {"F3", "Save & Close"}, {"Esc", "Cancel"}}));
            rows.push_back(ErrorLine(state_->form_error));
            return CheckInWindow(state_, ftxui::vbox(std::move(rows)));
        }

    private:
        AppState* state_;
        StationFieldInputs inputs_;
        ftxui::Component remarks_input_;
    };

    ftxui::Component BuildEditNetPage(AppState* state)
    {
        ftxui::Component input_name =
            ftxui::Input(&state->edit_net_name, "e.g. Weekly Skywarn Net", SingleLineInputOption());
        // An Amateur Radio net's radio fields, or a GMRS net's Channel.
        ftxui::Component input_mode =
            ftxui::Maybe(ModeToggle(state, &state->edit_net_mode_index), &state->edit_net_amateur);
        ftxui::Component input_frequency = ftxui::Maybe(ftxui::Input(&state->edit_net_frequency, "MHz, e.g. 146.940",
                                                                     FrequencyInputOption(&state->edit_net_frequency)),
                                                        &state->edit_net_amateur);
        ftxui::Component input_offset = ftxui::Maybe(
            ftxui::Input(&state->edit_net_offset, "e.g. -0.6 (optional)", OffsetInputOption(&state->edit_net_offset)),
            &state->edit_net_amateur);
        ftxui::Component channel =
            ftxui::Maybe(std::make_shared<GmrsChannelPicker>(&state->edit_net_gmrs_channel), &state->edit_net_gmrs);
        ftxui::Component input_tone =
            ftxui::Input(&state->edit_net_tone, "e.g. 100.0 (optional)", FrequencyInputOption(&state->edit_net_tone));
        ftxui::InputOption location_option = SingleLineInputOption();
        location_option.on_change = ZipCodeFieldHandler(&state->edit_net_location);
        ftxui::Component input_location =
            ftxui::Input(&state->edit_net_location, "ZIP/postal (optional)", location_option);
        ftxui::Component input_recurrence =
            ftxui::Input(&state->edit_net_recurrence, "e.g. Tuesdays 8pm ET", SingleLineInputOption());
        ftxui::Component input_comments =
            ftxui::Input(&state->edit_net_comments, "Anything (optional)", SingleLineInputOption());
        ftxui::Component partial_match =
            ftxui::Maybe(PartialMatchToggle(state, &state->edit_net_partial_match_index), &state->edit_net_amateur);

        ftxui::MenuOption saved_station_menu_option;
        saved_station_menu_option.focused_entry = &state->selected_saved_station_index;
        saved_station_menu_option.on_enter = LoadSavedStationHandler(state);
        saved_station_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component saved_station_menu =
            ClickableList(state, ftxui::Menu(&state->edit_net_saved_station_labels,
                                             &state->selected_saved_station_index, saved_station_menu_option));

        // Not there at all when the user may only log the net (see
        // AppState::edit_net_editable): nothing to focus or type into.
        for (ftxui::Component* field :
             {&input_name, &input_mode, &input_frequency, &input_offset, &channel, &input_tone, &input_location,
              &input_recurrence, &input_comments, &partial_match})
        {
            *field = ftxui::Maybe(*field, &state->edit_net_editable);
        }
        ftxui::Component root = ftxui::Container::Vertical({
            input_name,
            input_mode,
            input_frequency,
            input_offset,
            channel,
            input_tone,
            input_location,
            input_recurrence,
            input_comments,
            partial_match,
            saved_station_menu,
        });
        state->edit_net_name_input = input_name;
        ftxui::Component main_view = ftxui::Renderer(
            root, EditNetRenderer(state, input_name, input_mode, input_frequency, input_offset, channel, input_tone,
                                  input_location, input_recurrence, input_comments, partial_match, saved_station_menu));

        ftxui::InputOption callsign_option = SingleLineInputOption();
        callsign_option.on_change = SavedStationCallsignChangeHandler(state);
        callsign_option.on_enter = SavedStationCallsignEnterHandler(state);
        ftxui::Component callsign_input =
            ftxui::Input(&state->saved_station.callsign, "Callsign (? = unsure)", callsign_option);
        state->saved_station_callsign_input = callsign_input;
        StationFieldInputs station_inputs = BuildStationFieldInputs(&state->saved_station, callsign_input);
        ftxui::Component remarks_input =
            ftxui::Input(&state->saved_station_remarks, "Optional", SingleLineInputOption());

        ftxui::Components modal_components = StationFieldComponents(station_inputs);
        modal_components.push_back(remarks_input);
        ftxui::Component modal_view = ftxui::Renderer(ftxui::Container::Vertical(modal_components),
                                                      SavedStationModalRenderer(state, station_inputs, remarks_input));

        ftxui::Component with_station_modal = LayeredModal(main_view, modal_view, &state->show_saved_station_modal);
        ftxui::Component with_zmodem_modal =
            LayeredModal(with_station_modal, BuildZmodemConfirmModal(state), &state->show_zmodem_confirm_modal);
        return WithRowDeleteConfirm(state, LayeredModal(with_zmodem_modal, BuildDeleteNetConfirmModal(state),
                                                        &state->show_delete_net_confirm_modal));
    }

    // ---- Import net page ---------------------------------------------------

    // What the Import page says while it has no files to list.
    static std::string EmptyImportListHint(AppState* state, const std::string& pattern, const std::string** command)
    {
        *command = nullptr;
        if (CanPullUpstream(state) && !(state->import_session && state->import_session_ad_hoc))
        {
            return "No " + pattern + " files in imports/ yet. Press F4 to pull one from " +
                   state->settings.upstream_host + ".";
        }
        if (state->ssh_username.empty())
        {
            return "No " + pattern + " files in imports/ yet. Press F3 to receive one via ZMODEM.";
        }
        if (state->over_mosh || NoZmodemOnThisSystem() || SessionPrefersSftp(state))
        {
            const std::string& upload = ScpUploadCommand(state);
            if (upload.empty())
            {
                return "No " + pattern + " files received yet. Upload one with scp or sftp to /imports.";
            }
            // The command goes on its own line, below the hint.
            *command = &upload;
            return "No " + pattern + " files received yet. Upload one with this command:";
        }
        return "No " + pattern + " files received yet. Press F3 to receive one via ZMODEM.";
    }

    // A hint, with a command under it if it has one.
    static ftxui::Element HintWithCommand(const std::string& hint, const std::string* command)
    {
        if (command == nullptr)
        {
            return HintText(hint);
        }
        return ftxui::vbox({HintText(hint), CommandBlock(*command)});
    }

    class ImportNetRenderer
    {
    public:
        ImportNetRenderer(AppState* state, ftxui::Component file_menu) : state_(state), file_menu_(std::move(file_menu))
        {
        }

        ftxui::Element operator()() const
        {
            bool session = state_->import_session;
            std::string pattern = session ? "*.qlsession" : "*.qlnet";
            ftxui::Element file_list;
            if (state_->import_net_files.empty())
            {
                const std::string* command = nullptr;
                std::string hint = EmptyImportListHint(state_, pattern, &command);
                file_list = HintWithCommand(hint, command);
            }
            else
            {
                file_list = file_menu_->Render() | ftxui::yframe | ftxui::vscroll_indicator;
            }

            ftxui::Elements rows;
            if (session)
            {
                rows.push_back(
                    HintParagraph(state_->import_session_ad_hoc
                                      ? "A session exported with F7 (its .qlsession file) becomes a new ad hoc "
                                        "net here."
                                      : "A session exported with F7 (its .qlsession file) is added to " +
                                            state_->import_session_net_name + "'s history."));
            }
            rows.push_back(Heading("Files ready to import:"));
            rows.push_back(Framed(file_list) | ftxui::flex);
            // Where there's no ZMODEM, the command stays on screen after files
            // have arrived, for the next one (an empty list shows it above).
            if (!state_->import_net_files.empty() && !state_->ssh_username.empty() &&
                (state_->over_mosh || NoZmodemOnThisSystem() || SessionPrefersSftp(state_)))
            {
                const std::string& upload = ScpUploadCommand(state_);
                if (!upload.empty())
                {
                    static const std::string kUploadAnother = "Upload another with this command:";
                    rows.push_back(HintWithCommand(kUploadAnother, &upload));
                }
            }
            rows.push_back(StatusLine(state_->status_message));
            rows.push_back(ErrorLine(state_->form_error));

            std::string title = !session                        ? "Import Net"
                                : state_->import_session_ad_hoc ? "Import Ad Hoc Session"
                                                                : "Import Session: " + state_->import_session_net_name;
            std::vector<KeyHint> hints;
            hints.push_back({"F2/Enter", "Import"});
            // No ZMODEM at a local terminal: there's no one to receive from.
            // Nor where the system has none (Windows), or over Mosh.
            if (CanReceiveZmodem(state_))
            {
                hints.push_back({"F3", "Receive (ZMODEM)"});
            }
            // From the upstream server, set up in Settings; ad hoc nets have
            // no History there to pull from.
            if (CanPullUpstream(state_) && !(session && state_->import_session_ad_hoc))
            {
                hints.push_back({"F4", "Pull"});
            }
            hints.push_back({"Esc", "Back"});
            return PageChrome(title, ftxui::vbox(std::move(rows)), hints);
        }

    private:
        AppState* state_;
        ftxui::Component file_menu_;
    };

    // The Pull window (see PullStage): the upstream's nets, to pick the one
    // to pull.
    class PullModalRenderer
    {
    public:
        explicit PullModalRenderer(AppState* state) : state_(state) {}

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(Heading(state_->pull_sessions ? "Pull Sessions" : "Pull Net"));
            rows.push_back(DialogSeparator());
            if (state_->pull_stage == PullStage::kChoosing)
            {
                rows.push_back(ftxui::paragraph(state_->pull_sessions
                                                    ? std::string("Select an upstream net to pull sessions from:")
                                                    : std::string("Select an upstream net to import:")) |
                               ftxui::bold | ftxui::color(kColorLabel));
                if (state_->pull_sessions)
                {
                    rows.push_back(ftxui::paragraph("Adds its closed sessions to " + state_->import_session_net_name +
                                                    "'s History; duplicates are skipped.") |
                                   ftxui::color(kColorHint));
                }
                rows.push_back(NetList());
                rows.push_back(DialogSeparator());
                rows.push_back(KeyHintRow({{"F2/Enter", "Pull"}, {"Esc", "Cancel"}}));
            }
            else
            {
                std::string working = state_->pull_stage == PullStage::kFetching
                                          ? "Fetching " + state_->pull_net_name + " from upstream..."
                                          : std::string("Asking upstream for its nets...");
                rows.push_back(ftxui::paragraph(working) | ftxui::color(kColorLabel));
                rows.push_back(DialogSeparator());
                rows.push_back(KeyHintRow({{"Esc", "Cancel"}}));
            }
            rows.push_back(ErrorLine(state_->form_error));
            int width = std::max(40, std::min(FrameTerminalSize().dimx - 4, 80));
            return ftxui::vbox(std::move(rows)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width) |
                   ftxui::color(kColorHeading) | ftxui::borderStyled(kColorDialogBorder);
        }

    private:
        // The nets, the highlighted one marked and kept in view.
        ftxui::Element NetList() const
        {
            ftxui::Elements rows;
            for (std::size_t i = 0; i < state_->pull_net_labels.size(); ++i)
            {
                bool marked = static_cast<int>(i) == state_->selected_pull_net;
                ftxui::Element row =
                    ftxui::hbox({ftxui::text(marked ? "> " : "  "),
                                 ftxui::text(state_->pull_net_labels[i]) | ftxui::color(kColorListRow)});
                rows.push_back(marked ? row | ftxui::inverted | ftxui::focus : row);
            }
            // The window's other rows: its border, title, two paragraphs, the
            // list's frame, a separator, the keys and the error line.
            int room = std::max(2, FrameTerminalSize().dimy - 14);
            int height = std::min(static_cast<int>(rows.size()), room);
            return DialogFramed(ftxui::vbox(std::move(rows)) | ftxui::yframe | ftxui::vscroll_indicator |
                                ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, height));
        }

        AppState* state_;
    };

    // The Import or Merge window (see MergeStage): first the nets here that
    // the file's looks like, then what merging into the chosen one would do.
    class NetMergeModalRenderer
    {
    public:
        explicit NetMergeModalRenderer(AppState* state) : state_(state) {}

        ftxui::Element operator()() const
        {
            ftxui::Elements rows = state_->merge_stage == MergeStage::kSummary ? SummaryRows() : ChoiceRows();
            int width = WindowWidth();
            return ftxui::vbox(std::move(rows)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width) |
                   ftxui::color(kColorHeading) | ftxui::borderStyled(kColorDialogBorder);
        }

    private:
        // Wider on a wide terminal, so more of each difference shows.
        static int WindowWidth()
        {
            return std::max(40, std::min(FrameTerminalSize().dimx - 4, 100));
        }

        // A list in the window, the highlighted row marked and kept in view.
        // "Keep" or "Replace", always its full width: a long row is cut at
        // its end, never here.
        static ftxui::Element ReplaceLabel(bool replace)
        {
            return ftxui::text(replace ? "Replace" : "Keep") |
                   ftxui::color(replace ? ftxui::Color(kColorDanger) : ftxui::Color(kColorData)) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 7);
        }

        // `lists`: how many lists share the window's room. `line_count`:
        // the lines in all of `lines`, when an entry has more than one.
        ftxui::Element List(const std::vector<ftxui::Element>& lines, int selected, int lists = 1,
                            int line_count = -1) const
        {
            ftxui::Elements rows;
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                bool marked = static_cast<int>(i) == selected;
                ftxui::Element row = ftxui::hbox({ftxui::text(marked ? "> " : "  "), lines[i]});
                rows.push_back(marked ? row | ftxui::inverted | ftxui::focus : row);
            }
            // The window's other rows: 13 around one list, and about 4 more
            // (a heading and a frame) for each further one.
            int room = std::max(2, (FrameTerminalSize().dimy - 13 - 4 * (lists - 1)) / lists);
            int height = std::min(line_count >= 0 ? line_count : static_cast<int>(lines.size()), room);
            return DialogFramed(ftxui::vbox(std::move(rows)) | ftxui::yframe | ftxui::vscroll_indicator |
                                ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, height));
        }

        ftxui::Elements ChoiceRows() const
        {
            const std::string& name = state_->merge_slice.net.name;
            ftxui::Elements rows;
            rows.push_back(Heading("Import or Merge?"));
            rows.push_back(DialogSeparator());
            rows.push_back(
                ftxui::paragraph(state_->merge_name_taken
                                     ? "You already have a net named \"" + name +
                                           "\", so this file can't be added as a new net."
                                     : "This file's net, \"" + name + "\", looks like one you already have.") |
                ftxui::bold | ftxui::color(kColorLabel));
            rows.push_back(ftxui::paragraph("Merge adds the file's sessions and saved stations that the "
                                            "highlighted net doesn't have yet, and shows what it will do "
                                            "first.") |
                           ftxui::color(kColorHint));
            std::vector<ftxui::Element> lines;
            for (const std::string& label : state_->merge_candidate_labels)
            {
                lines.push_back(ftxui::text(label) | ftxui::color(kColorListRow));
            }
            rows.push_back(List(lines, state_->selected_merge_candidate));
            rows.push_back(DialogSeparator());
            if (state_->merge_name_taken)
            {
                rows.push_back(KeyHintRow({{"F3", "Merge"}, {"Esc", "Cancel"}}));
            }
            else
            {
                rows.push_back(KeyHintRow({{"F2/Enter", "Import New"}, {"F3", "Merge"}, {"Esc", "Cancel"}}));
            }
            return rows;
        }

        ftxui::Elements SummaryRows() const
        {
            const NetMergePlan& plan = state_->merge_plan;
            int added = 0;
            int here = 0;
            int open_added = 0;
            for (const MergeSession& session : plan.sessions)
            {
                if (session.kind == MergeSessionKind::kNew)
                {
                    ++added;
                    open_added += session.file_open ? 1 : 0;
                }
                here += session.kind == MergeSessionKind::kAlreadyHere ? 1 : 0;
            }
            int total = static_cast<int>(plan.sessions.size());

            ftxui::Elements rows;
            rows.push_back(Heading("Merge into " + state_->merge_target_name + "?"));
            rows.push_back(DialogSeparator());
            rows.push_back(ftxui::paragraph("Adds " + std::to_string(added) + " of the file's " +
                                            Plural(total, "session", "sessions") + " and " +
                                            Plural(plan.new_saved_stations, "saved station", "saved stations") + ".") |
                           ftxui::bold | ftxui::color(kColorLabel));
            // A line each: the sessions already here, and the stations.
            std::string sessions_line;
            if (here > 0)
            {
                sessions_line = Plural(here, "session is", "sessions are") + " here already";
            }
            if (open_added > 0)
            {
                sessions_line.append(sessions_line.empty() ? "" : "; ");
                sessions_line.append(std::to_string(open_added));
                sessions_line.append(open_added == 1 ? " still open in the file comes in closed"
                                                     : " still open in the file come in closed");
            }
            if (!sessions_line.empty())
            {
                sessions_line.push_back('.');
                rows.push_back(ftxui::paragraph(sessions_line) | ftxui::color(kColorHint));
            }
            rows.push_back(ftxui::paragraph("Local stations keep their details and remarks; blanks are filled "
                                            "in.") |
                           ftxui::color(kColorHint));

            int list_count = (state_->merge_conflicts.empty() ? 0 : 1) + (plan.station_conflicts.empty() ? 0 : 1);
            if (!state_->merge_conflicts.empty())
            {
                rows.push_back(ftxui::paragraph(state_->merge_conflicts.size() == 1
                                                    ? "1 session differs. Keep yours, or Replace it with "
                                                      "the file's:"
                                                    : std::to_string(state_->merge_conflicts.size()) +
                                                          " sessions differ. Keep yours, or Replace them "
                                                          "with the file's:") |
                               ftxui::color(kColorLabel));
                std::vector<ftxui::Element> lines;
                // A row's room: the window less its border, the list's
                // frame and the "> " marker.
                int row_room = WindowWidth() - 6;
                int line_count = 0;
                lines.reserve(state_->merge_conflicts.size());
                for (std::size_t i = 0; i < state_->merge_conflicts.size(); ++i)
                {
                    const MergeSession& session = plan.sessions[state_->merge_conflicts[i]];
                    const std::string& when = state_->merge_conflict_texts[i].when;
                    const std::string& what = state_->merge_conflict_texts[i].what;
                    // The highlighted session, if what differs doesn't fit
                    // beside it, says so in full on a line of its own.
                    bool highlighted = static_cast<int>(i) == state_->selected_merge_conflict;
                    int row_width = 7 + 2 + static_cast<int>(when.size()) + 3 + TextWidth(what);
                    if (highlighted && row_width > row_room)
                    {
                        lines.push_back(ftxui::vbox({
                            ftxui::hbox({
                                ReplaceLabel(session.replace),
                                ftxui::text("  " + when) | ftxui::color(kColorListRow),
                            }),
                            ftxui::hbox({
                                ftxui::text("         "),
                                ftxui::paragraph(what) | ftxui::color(kColorListRow) | ftxui::flex,
                            }),
                        }));
                        // The paragraph's lines, at the room it has.
                        int text_room = std::max(1, row_room - 9);
                        line_count += 1 + (TextWidth(what) + text_room - 1) / text_room;
                        continue;
                    }
                    lines.push_back(ftxui::hbox({
                        ReplaceLabel(session.replace),
                        ftxui::text("  " + when + "   " + what) | ftxui::color(kColorListRow),
                    }));
                    ++line_count;
                }
                rows.push_back(List(lines, state_->selected_merge_conflict, list_count, line_count));
            }
            // Then the stations whose details differ; one highlight moves
            // through both lists.
            if (!plan.station_conflicts.empty())
            {
                rows.push_back(ftxui::paragraph(plan.station_conflicts.size() == 1
                                                    ? "1 station differs. Keep yours, or Replace it with the "
                                                      "file's:"
                                                    : std::to_string(plan.station_conflicts.size()) +
                                                          " stations differ. Keep yours, or Replace them with "
                                                          "the file's:") |
                               ftxui::color(kColorLabel));
                // Each station: Keep/Replace and its callsign, then a line
                // for each detail that differs.
                std::vector<ftxui::Element> lines;
                lines.reserve(plan.station_conflicts.size());
                int line_count = 0;
                for (std::size_t c = 0; c < plan.station_conflicts.size(); ++c)
                {
                    const std::vector<std::string>& differences = state_->merge_station_difference_texts[c];
                    ftxui::Elements station_lines;
                    station_lines.reserve(differences.size() + 1);
                    station_lines.push_back(ftxui::hbox({
                        ReplaceLabel(plan.station_conflicts[c].replace),
                        ftxui::text(state_->merge_station_callsign_texts[c]) | ftxui::color(kColorListRow),
                    }));
                    for (const std::string& difference : differences)
                    {
                        station_lines.push_back(ftxui::text(difference) | ftxui::color(kColorListRow));
                    }
                    line_count += static_cast<int>(station_lines.size());
                    lines.push_back(ftxui::vbox(std::move(station_lines)));
                }
                rows.push_back(List(lines,
                                    state_->selected_merge_conflict - static_cast<int>(state_->merge_conflicts.size()),
                                    list_count, line_count));
            }
            rows.push_back(DialogSeparator());
            bool anything = added > 0 || plan.new_saved_stations > 0 || !state_->merge_conflicts.empty() ||
                            plan.known_saved_stations > 0 || !plan.station_conflicts.empty();
            if (anything)
            {
                rows.push_back(HintText("A merge can't be undone."));
                // With something to choose, how to choose it, here with the
                // other keys rather than in each list's heading.
                if (list_count > 0)
                {
                    rows.push_back(KeyHintRow(
                        {{"F2", "Merge"}, {"Up/Down", "Choose"}, {"Left/Right", "Keep/Replace"}, {"Esc", "Back"}}));
                }
                else
                {
                    rows.push_back(KeyHintRow({{"F2", "Merge"}, {"Esc", "Back"}}));
                }
            }
            else
            {
                rows.push_back(HintText("Nothing to merge: this net has everything in the file."));
                rows.push_back(KeyHintRow({{"Esc", "Back"}}));
            }
            return rows;
        }

        // "1 session", "3 sessions".
        static std::string Plural(int count, const char* singular, const char* plural)
        {
            return std::to_string(count) + " " + (count == 1 ? singular : plural);
        }

        AppState* state_;
    };

    ftxui::Component BuildImportNetPage(AppState* state)
    {
        ftxui::MenuOption file_menu_option;
        file_menu_option.on_enter = ImportSelectedNetSliceHandler(state);
        file_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component file_menu = ClickableList(
            state, ftxui::Menu(&state->import_net_files, &state->selected_import_file_index, file_menu_option));

        ftxui::Component root = ftxui::Container::Vertical({file_menu});
        ftxui::Component main_view = ftxui::Renderer(root, ImportNetRenderer(state, file_menu));
        ftxui::Component merge_modal = ftxui::Renderer(ftxui::Container::Vertical({}), NetMergeModalRenderer(state));
        ftxui::Component with_merge = LayeredModal(main_view, merge_modal, &state->show_merge_modal);
        ftxui::Component pull_modal = ftxui::Renderer(ftxui::Container::Vertical({}), PullModalRenderer(state));
        ftxui::Component with_pull = LayeredModal(with_merge, pull_modal, &state->show_pull_modal);
        return LayeredModal(with_pull, BuildZmodemConfirmModal(state), &state->show_zmodem_confirm_modal);
    }

    // ---- Manage users page --------------------------------------------------

    // For the console and SSH Admins (see CanManageUsers) -- reached from
    // Settings' F4 at the console, F5 over SSH. Lists who's allowed to SSH
    // in and lets them add/remove entries. Every action rechecks the
    // Admin level in the database (RefuseNonAdmin).
    class ManageUsersRenderer
    {
    public:
        ManageUsersRenderer(AppState* state, ftxui::Component user_menu, ftxui::Component input_username,
                            ftxui::Component input_amateur, ftxui::Component input_gmrs,
                            ftxui::Component input_public_key, ftxui::Component access_toggle)
            : state_(state),
              user_menu_(std::move(user_menu)),
              input_username_(std::move(input_username)),
              input_amateur_(std::move(input_amateur)),
              input_gmrs_(std::move(input_gmrs)),
              input_public_key_(std::move(input_public_key)),
              access_toggle_(std::move(access_toggle))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element user_list =
                state_->manage_users.empty()
                    ? HintText("No SSH users yet.")
                    : ftxui::vbox({
                          PickableColumnHeader(state_, PickList::kUsers, UserListHeader(state_->list_width)),
                          PickableRows(state_, PickList::kUsers, state_->manage_users_labels,
                                       state_->selected_user_index, user_menu_) |
                              ftxui::yframe | ftxui::vscroll_indicator,
                      });

            ftxui::Element content = ftxui::vbox({
                Heading("SSH Users:"),
                Framed(user_list) | ftxui::flex,
                PickPrompt(state_, PickList::kUsers),
                Separator(),
                ftxui::hbox({FieldLabel("Username:      "), input_username_->Render()}),
                ftxui::hbox({FieldLabel("Amateur Call:  "), input_amateur_->Render()}),
                ftxui::hbox({FieldLabel("GMRS Call:     "), input_gmrs_->Render()}),
                ftxui::hbox({FieldLabel("Public Key:    "), input_public_key_->Render()}),
                ftxui::hbox({FieldLabel("Access:        "), access_toggle_->Render()}),
                HintParagraph("A username is any login name. Give one call sign or both; each service's nets "
                              "are logged with its own, and watched without one."),
                HintParagraph("Paste a full authorized_keys-style line, e.g. from "
                              "~/.ssh/id_ed25519.pub -- \"ssh-ed25519 AAAA... comment\"."),
                HintParagraph("View-only watches and exports, nothing else. A Net Admin looks after the nets "
                              "they're given; an Admin also manages users. F4 (or Enter) edits a user."),
                StatusLine(state_->status_message),
                ErrorLine(state_->form_error),
            });

            // The Keys window shows its own keys.
            if (state_->show_user_keys_modal)
            {
                return PageChrome("Manage Users", content, {});
            }
            if (state_->row_pick_action != RowPickAction::kNone)
            {
                return PageChrome("Manage Users", content, PickKeyHints(state_));
            }
            return PageChrome("Manage Users", content,
                              {{"F2", "Add"},
                               {"F3", "Remove"},
                               {"F4", "Edit"},
                               {"F5", state_->manage_self_service_on ? "Own Keys: On" : "Own Keys: Off"},
                               {"F6", "Key Log"},
                               {"F7", "Net Access"},
                               {"F8", state_->manage_restricted_on ? "Restricted: On" : "Restricted: Off"},
                               {"Esc", "Back"}});
        }

    private:
        AppState* state_;
        ftxui::Component user_menu_;
        ftxui::Component input_username_;
        ftxui::Component input_amateur_;
        ftxui::Component input_gmrs_;
        ftxui::Component input_public_key_;
        ftxui::Component access_toggle_;
    };

    // The Edit User window over Manage Users (F4, or Enter on a user):
    // their username and access, saved together with F2, and their keys,
    // added and removed there and then.
    class UserKeysModalRenderer
    {
    public:
        UserKeysModalRenderer(AppState* state, ftxui::Component input_username, ftxui::Component input_amateur,
                              ftxui::Component input_gmrs, ftxui::Component access_toggle, ftxui::Component key_menu,
                              ftxui::Component input_key)
            : state_(state),
              input_username_(std::move(input_username)),
              input_amateur_(std::move(input_amateur)),
              input_gmrs_(std::move(input_gmrs)),
              access_toggle_(std::move(access_toggle)),
              key_menu_(std::move(key_menu)),
              input_key_(std::move(input_key))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(Heading("Edit User: " + state_->user_keys_username));
            rows.push_back(DialogSeparator());
            rows.push_back(ftxui::hbox({FieldLabel("Username:   "), input_username_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Amateur:    "), input_amateur_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("GMRS:       "), input_gmrs_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Access:     "), access_toggle_->Render()}));
            rows.push_back(
                HintParagraph("F2 saves these; they apply from the user's next "
                              "login. Renaming moves their settings and files too."));
            rows.push_back(DialogSeparator());
            rows.push_back(Heading("Keys:"));
            rows.push_back(Framed(ftxui::vbox({
                PickableColumnHeader(state_, PickList::kUserKeys, UserKeyListHeader(state_->list_width)),
                PickableRows(state_, PickList::kUserKeys, state_->user_keys_labels, state_->selected_user_key_index,
                             key_menu_) |
                    ftxui::yframe | ftxui::vscroll_indicator | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 8),
            })));
            rows.push_back(PickPrompt(state_, PickList::kUserKeys));
            if (IsPicking(state_, PickList::kUserKeys) || state_->user_keys.empty())
            {
                // The picking prompt has the row.
            }
            else
            {
                rows.push_back(HintText(UserKeyDetail(state_)));
            }
            rows.push_back(ftxui::hbox({FieldLabel("Add a key:  "), input_key_->Render()}));
            rows.push_back(DialogSeparator());
            if (IsPicking(state_, PickList::kUserKeys))
            {
                rows.push_back(KeyHintRow(PickKeyHints(state_)));
            }
            else
            {
                rows.push_back(KeyHintRow({{"F2", "Save"},
                                           {"F3", "Remove"},
                                           {"F4", "Add"},
                                           {"F5", UserKeyOffLabel(state_)},
                                           {"F6", UserKeysOffLabel(state_)},
                                           {"Esc", "Cancel"}}));
            }
            rows.push_back(StatusLine(state_->status_message));
            rows.push_back(ErrorLine(state_->form_error));
            return CheckInWindow(state_, ftxui::vbox(std::move(rows)));
        }

    private:
        AppState* state_;
        ftxui::Component input_username_;
        ftxui::Component input_amateur_;
        ftxui::Component input_gmrs_;
        ftxui::Component access_toggle_;
        ftxui::Component key_menu_;
        ftxui::Component input_key_;
    };

    // The key log over Manage Users (F6): the last changes to who can log in.
    class KeyLogWindowRenderer
    {
    public:
        KeyLogWindowRenderer(AppState* state, ftxui::Component log_menu) : state_(state), log_menu_(std::move(log_menu))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(Heading("Key Log"));
            rows.push_back(DialogSeparator());
            if (state_->key_log_labels.empty())
            {
                rows.push_back(HintText("Nothing yet."));
            }
            else
            {
                rows.push_back(Framed(ftxui::vbox({
                    ColumnHeader(KeyLogListHeader(state_->list_width)),
                    log_menu_->Render() | ftxui::yframe | ftxui::vscroll_indicator |
                        ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 12),
                })));
            }
            rows.push_back(DialogSeparator());
            rows.push_back(KeyHintRow({{"F7", "Export"}, {"Up/Down", "Scroll"}, {"Esc", "Close"}}));
            rows.push_back(StatusLine(state_->status_message));
            rows.push_back(ErrorLine(state_->form_error));
            return CheckInWindow(state_, ftxui::vbox(std::move(rows)));
        }

    private:
        AppState* state_;
        ftxui::Component log_menu_;
    };

    ftxui::Component BuildManageUsersPage(AppState* state)
    {
        ftxui::MenuOption user_menu_option;
        user_menu_option.on_enter = ShowUserKeysHandler(state);
        user_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component user_menu = ClickableList(
            state, ftxui::Menu(&state->manage_users_labels, &state->selected_user_index, user_menu_option));
        ftxui::Component input_username =
            ftxui::Input(&state->new_user_username, "Their login name", SingleLineInputOption());
        ftxui::InputOption amateur_option = SingleLineInputOption();
        amateur_option.on_change = UppercaseFieldHandler(&state->new_user_amateur_callsign);
        ftxui::Component input_amateur =
            ftxui::Input(&state->new_user_amateur_callsign, "e.g. W4KWK (optional)", amateur_option);
        ftxui::InputOption gmrs_option = SingleLineInputOption();
        gmrs_option.on_change = UppercaseFieldHandler(&state->new_user_gmrs_callsign);
        ftxui::Component input_gmrs =
            ftxui::Input(&state->new_user_gmrs_callsign, "e.g. WSIP663 (optional)", gmrs_option);
        ftxui::Component input_public_key = ftxui::Input(&state->new_user_public_key, "ssh-ed25519 AAAA... comment",
                                                         PublicKeyInputOption(&state->new_user_public_key));

        ftxui::MenuOption access_option = ftxui::MenuOption::Toggle();
        access_option.entries_option.transform = ToggleEntryTransform;
        access_option.elements_infix = ToggleGap;
        access_option.focused_entry = &state->new_user_access_index;
        ftxui::Component access_toggle = std::make_shared<IgnoreTab>(
            ftxui::Menu(&state->new_user_access_labels, &state->new_user_access_index, access_option));

        ftxui::Component root = ftxui::Container::Vertical(
            {user_menu, input_username, input_amateur, input_gmrs, input_public_key, access_toggle});
        ftxui::Component main_view =
            ftxui::Renderer(root, ManageUsersRenderer(state, user_menu, input_username, input_amateur, input_gmrs,
                                                      input_public_key, access_toggle));

        ftxui::Component input_rename =
            ftxui::Input(&state->rename_username, "Their login name", SingleLineInputOption());
        ftxui::InputOption edit_amateur_option = SingleLineInputOption();
        edit_amateur_option.on_change = UppercaseFieldHandler(&state->edit_user_amateur_callsign);
        ftxui::Component input_edit_amateur =
            ftxui::Input(&state->edit_user_amateur_callsign, "e.g. W4KWK (optional)", edit_amateur_option);
        ftxui::InputOption edit_gmrs_option = SingleLineInputOption();
        edit_gmrs_option.on_change = UppercaseFieldHandler(&state->edit_user_gmrs_callsign);
        ftxui::Component input_edit_gmrs =
            ftxui::Input(&state->edit_user_gmrs_callsign, "e.g. WSIP663 (optional)", edit_gmrs_option);
        ftxui::MenuOption edit_access_option = ftxui::MenuOption::Toggle();
        edit_access_option.entries_option.transform = ToggleEntryTransform;
        edit_access_option.elements_infix = ToggleGap;
        edit_access_option.focused_entry = &state->edit_user_access_index;
        ftxui::Component edit_access_toggle = std::make_shared<IgnoreTab>(
            ftxui::Menu(&state->new_user_access_labels, &state->edit_user_access_index, edit_access_option));
        ftxui::MenuOption key_menu_option;
        key_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component key_menu = ClickableList(
            state, ftxui::Menu(&state->user_keys_labels, &state->selected_user_key_index, key_menu_option));
        ftxui::InputOption key_option = PublicKeyInputOption(&state->new_key_text);
        key_option.on_enter = AddUserKeyHandler(state);
        ftxui::Component input_key = ftxui::Input(&state->new_key_text, "ssh-ed25519 AAAA... comment", key_option);
        ftxui::Component keys_modal =
            ftxui::Renderer(ftxui::Container::Vertical({input_rename, input_edit_amateur, input_edit_gmrs,
                                                        edit_access_toggle, key_menu, input_key}),
                            UserKeysModalRenderer(state, input_rename, input_edit_amateur, input_edit_gmrs,
                                                  edit_access_toggle, key_menu, input_key));

        ftxui::MenuOption log_menu_option;
        log_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component log_menu =
            ftxui::Menu(&state->key_log_labels, &state->selected_key_log_index, log_menu_option);
        ftxui::Component log_modal = ftxui::Renderer(log_menu, KeyLogWindowRenderer(state, log_menu));

        // The key log's F7 export ends in the same "Saved to ... open the folder" window as the other exports.
        ftxui::Component with_log = LayeredModal(LayeredModal(main_view, keys_modal, &state->show_user_keys_modal),
                                                 log_modal, &state->show_key_log_window);
        return WithRowDeleteConfirm(
            state, LayeredModal(with_log, BuildZmodemConfirmModal(state), &state->show_zmodem_confirm_modal));
    }

    // ---- Net Access page ------------------------------------------------------

    // Who has which nets (F7 on Manage Users, or a Net Admin's F5 on
    // Settings): first the net, then its people, [x] for those who have it.
    class NetAccessRenderer
    {
    public:
        NetAccessRenderer(AppState* state, ftxui::Component net_menu, ftxui::Component user_menu)
            : state_(state), net_menu_(std::move(net_menu)), user_menu_(std::move(user_menu))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            if (state_->net_access_stage == 1)
            {
                rows.push_back(Heading(state_->net_access_heading));
                rows.push_back(state_->net_access_users.empty()
                                   ? HintText("No full users yet. Add them in Manage Users.")
                                   : Framed(user_menu_->Render() | ftxui::yframe | ftxui::vscroll_indicator) |
                                         ftxui::flex);
                if (!state_->net_access_custodians.empty())
                {
                    rows.push_back(ftxui::hbox({FieldLabel("Net Admins: "), ftxui::text(state_->net_access_custodians) |
                                                                                ftxui::color(kColorData)}));
                }
                rows.push_back(HintText("Enter gives a user this net or takes it back."));
            }
            else
            {
                rows.push_back(Heading("Choose a net:"));
                rows.push_back(Framed(net_menu_->Render() | ftxui::yframe | ftxui::vscroll_indicator) | ftxui::flex);
                rows.push_back(HintText(state_->manage_restricted_on || state_->access.restricted
                                            ? "Restricted is on: users log only the nets they have."
                                            : "Restricted is off: every full user logs every net. "
                                              "Turn it on in Manage Users."));
            }
            rows.push_back(StatusLine(state_->status_message));
            rows.push_back(ErrorLine(state_->form_error));
            return PageChrome("Net Access", ftxui::vbox(std::move(rows)),
                              state_->net_access_stage == 1
                                  ? std::vector<KeyHint>{{"F2/Enter", "Give/Take"}, {"Esc", "Back"}}
                                  : std::vector<KeyHint>{{"F2/Enter", "Choose"}, {"Esc", "Back"}});
        }

    private:
        AppState* state_;
        ftxui::Component net_menu_;
        ftxui::Component user_menu_;
    };

    ftxui::Component BuildNetAccessPage(AppState* state)
    {
        ftxui::MenuOption net_option;
        net_option.on_enter = NetAccessChooseNetHandler(state);
        net_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component net_menu = ClickableList(
            state, ftxui::Menu(&state->net_access_net_labels, &state->selected_net_access_net, net_option));
        ftxui::MenuOption user_option;
        user_option.on_enter = NetAccessToggleUserHandler(state);
        user_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component user_menu = ClickableList(
            state, ftxui::Menu(&state->net_access_user_labels, &state->selected_net_access_user, user_option));
        ftxui::Component root = ftxui::Container::Tab({net_menu, user_menu}, &state->net_access_stage);
        return ftxui::Renderer(root, NetAccessRenderer(state, net_menu, user_menu));
    }

    // ---- Help and the seldom-used windows (see InfoWindow) -------------------

    // A Station Card line ("Name:           Ann Amateur"), colored like a
    // form: the label as field labels are, what's known as entered data is,
    // and what isn't ("(none)", "(not in the FCC or ISED data)", "none yet") as
    // hints are.
    static ftxui::Element StationCardLine(const std::string& line)
    {
        // The labels are padded to this width (see OpenStationCard).
        const std::size_t kLabelWidth = 16;
        if (line.size() <= kLabelWidth)
        {
            return ftxui::hbox(
                {ftxui::text(line) | ftxui::color(kColorLabel), ftxui::text("(none)") | ftxui::color(kColorHint)});
        }
        std::string value = line.substr(kLabelWidth);
        bool placeholder = value[0] == '(' || value == "none yet" || value == "no nets";
        return ftxui::hbox({
            ftxui::text(line.substr(0, kLabelWidth)) | ftxui::color(kColorLabel),
            ftxui::paragraph(value) | ftxui::color(placeholder ? kColorHint : kColorData),
        });
    }

    class InfoWindowRenderer
    {
    public:
        InfoWindowRenderer(AppState* state, ftxui::Component query_input)
            : state_(state), query_input_(std::move(query_input))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Dimensions terminal = FrameTerminalSize();
            int screen_width = terminal.dimx;
            int screen_height = terminal.dimy;

            ftxui::Elements rows;
            rows.push_back(Heading(state_->info_title));
            rows.push_back(DialogSeparator());
            if (state_->info_window == InfoWindow::kStationSearch)
            {
                rows.push_back(ftxui::hbox({FieldLabel("Callsign: "), query_input_->Render()}));
            }
            for (const std::string& line : state_->info_summary)
            {
                rows.push_back(state_->info_window == InfoWindow::kStationCard
                                   ? StationCardLine(line)
                                   : ftxui::paragraph(line) | ftxui::color(kColorLabel));
            }
            if (!state_->info_rows.empty())
            {
                ftxui::Elements lines;
                for (std::size_t i = 0; i < state_->info_rows.size(); ++i)
                {
                    bool marked = static_cast<int>(i) == state_->info_selected;
                    ftxui::Element line =
                        ftxui::text((marked ? "> " : "  ") + state_->info_rows[i]) | ftxui::color(kColorListRow);
                    lines.push_back(marked ? line | ftxui::focus : line);
                }
                // As tall as the screen allows -- counting the lines the
                // summary wraps onto, and everything else the window
                // draws -- so its key row always shows; the list scrolls to
                // keep the marked row in view.
                int text_width = std::max(20, screen_width - 6);
                int summary_lines = 0;
                for (const std::string& line : state_->info_summary)
                {
                    int length = TextWidth(line);
                    summary_lines += std::max(1, (length + text_width - 1) / text_width);
                }
                int other_rows = 9 + summary_lines + (state_->info_window == InfoWindow::kStationSearch ? 1 : 0);
                int room = std::max(3, screen_height - other_rows);
                int height = std::min(static_cast<int>(lines.size()), room);
                rows.push_back(DialogFramed(ftxui::vbox({
                    ColumnHeader("  " + state_->info_header),
                    ftxui::vbox(std::move(lines)) | ftxui::vscroll_indicator | ftxui::yframe |
                        ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, height),
                })));
            }
            rows.push_back(DialogSeparator());
            std::vector<KeyHint> keys;
            if (state_->info_rows.size() > 1)
            {
                keys.push_back({"Up/Down", "Scroll"});
            }
            if (state_->info_window == InfoWindow::kRegulars && !state_->info_rows.empty() && !state_->viewing_only)
            {
                keys.push_back({"Enter", "Check In"});
            }
            keys.push_back({"Esc", "Close"});
            rows.push_back(KeyHintRow(keys));
            return ftxui::vbox(std::move(rows)) | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, screen_width - 4) |
                   ftxui::color(kColorHeading) | ftxui::borderStyled(kColorDialogBorder);
        }

    private:
        AppState* state_;
        ftxui::Component query_input_;
    };

    ftxui::Component BuildInfoWindow(AppState* state)
    {
        // Only drawn (and only given keys) in the Find a Station window.
        ftxui::InputOption query_option = SingleLineInputOption();
        query_option.on_change = InfoQueryChangeHandler(state);
        ftxui::Component query_input = ftxui::Input(&state->info_query, "Part of a callsign", query_option);
        return ftxui::Renderer(ftxui::Container::Vertical({query_input}), InfoWindowRenderer(state, query_input));
    }

}  // namespace ql
