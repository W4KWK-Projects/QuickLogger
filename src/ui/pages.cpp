#include "pages.hpp"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <utility>

#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

#include "../date_utils.hpp"
#include "../uls_import.hpp"
#include "chrome.hpp"
#include "handlers.hpp"

namespace ql
{

    static ftxui::Element ErrorLine(const std::string& message)
    {
        // A paragraph, so a long message wraps rather than being cut off at
        // the edge of the screen.
        return message.empty() ? ftxui::text("")
                               : ftxui::paragraph(message) | ftxui::color(kColorError);
    }

    static ftxui::Element StatusLine(const std::string& message)
    {
        return message.empty() ? ftxui::text("")
                               : ftxui::paragraph(message) | ftxui::color(kColorSuccess);
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

    // Builds every StationFieldInputs field except `callsign` (the caller
    // supplies that, since it needs different wiring per form) bound to
    // `station`'s fields.
    static StationFieldInputs BuildStationFieldInputs(Station* station,
                                                      ftxui::Component callsign_input)
    {
        StationFieldInputs inputs;
        inputs.callsign = std::move(callsign_input);
        inputs.name = ftxui::Input(&station->name, "Name", SingleLineInputOption());
        inputs.member_id = ftxui::Input(&station->member_id, "Member ID", SingleLineInputOption());
        inputs.street_address =
            ftxui::Input(&station->street_address, "Street Address", SingleLineInputOption());
        inputs.city = ftxui::Input(&station->city, "City", SingleLineInputOption());
        inputs.county = ftxui::Input(&station->county, "County", SingleLineInputOption());
        inputs.state = ftxui::Input(&station->state, "State", SingleLineInputOption());
        inputs.zip = ftxui::Input(&station->zip, "Zip", SingleLineInputOption());
        inputs.grid_square =
            ftxui::Input(&station->grid_square, "Grid Square", SingleLineInputOption());
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
        rows.push_back(
            ftxui::hbox({FieldLabel("Street Addr:   "), inputs.street_address->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("City:          "), inputs.city->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("County:        "), inputs.county->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("State:         "), inputs.state->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("Zip:           "), inputs.zip->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("Grid Square:   "), inputs.grid_square->Render()}));
        return rows;
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
            rows.push_back(Heading("ZMODEM"));
            rows.push_back(DialogSeparator());
            if (state_->zmodem_action == ZmodemAction::kSend)
            {
                rows.push_back(ftxui::text("Ready to send:"));
                rows.push_back(ftxui::text("  " + state_->zmodem_confirm_path) |
                               ftxui::color(kColorLabel));
                rows.push_back(ftxui::text(""));
                rows.push_back(
                    ftxui::text("Open your terminal's file-receive (ZMODEM) dialog now, then"));
                rows.push_back(ftxui::text("press Enter to start. Gives up after about 25s if"));
                rows.push_back(ftxui::text("nothing responds."));
                rows.push_back(DialogSeparator());
                rows.push_back(KeyHintRow({{"F2/Enter", "Send"}, {"Esc", "Skip"}}));
            }
            else
            {
                rows.push_back(ftxui::text("Ready to receive a file into imports/."));
                rows.push_back(ftxui::text(""));
                rows.push_back(ftxui::text("Press Enter now to start listening, THEN start"));
                rows.push_back(
                    ftxui::text("sending (uploading) the file from your terminal client."));
                rows.push_back(ftxui::text("Gives up after about 25s if nothing arrives."));
                rows.push_back(DialogSeparator());
                rows.push_back(KeyHintRow({{"F2/Enter", "Receive"}, {"Esc", "Cancel"}}));
            }

            return ftxui::vbox(rows) | ftxui::color(kColorHeading) |
                   ftxui::borderStyled(kColorDialogBorder);
        }

    private:
        AppState* state_;
    };

    static ftxui::Component BuildZmodemConfirmModal(AppState* state)
    {
        ftxui::Component root = ftxui::Container::Vertical({});
        return ftxui::Renderer(root, ZmodemConfirmModalRenderer(state));
    }

    // ---- Picking a row by number (see RowPickAction) ------------------------

    static bool IsPicking(const AppState* state, PickList list)
    {
        return state->row_pick_action != RowPickAction::kNone &&
               RowPickListFor(state->row_pick_action) == list;
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
        int widest = 9;
        for (int number : state->row_pick_numbers)
        {
            widest = number > widest ? number : widest;
        }
        return static_cast<int>(std::to_string(widest).size());
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

    // A list's rows: its Menu normally; in pick mode, every row with its
    // number beside it and the row about to be picked highlighted.
    static ftxui::Element PickableRows(const AppState* state, PickList list,
                                       const std::vector<std::string>& labels, int highlighted,
                                       const ftxui::Component& menu)
    {
        if (!IsPicking(state, list))
        {
            return menu->Render();
        }
        int width = PickNumberWidth(state);
        ftxui::Elements rows;
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
            else
            {
                std::string digits = i < state->row_pick_numbers.size()
                                         ? std::to_string(state->row_pick_numbers[i])
                                         : std::string();
                number = std::string(static_cast<std::size_t>(width) - digits.size(), ' ') +
                         digits + " ";
            }
            ftxui::Element row = ftxui::hbox({
                ftxui::text(number) | ftxui::color(kColorPickNumber),
                ftxui::text(rest) | ftxui::color(kColorListRow),
            });
            if (static_cast<int>(i) == highlighted)
            {
                row = row | ftxui::inverted | ftxui::focus;
            }
            rows.push_back(row);
        }
        return ftxui::vbox(rows);
    }

    // The prompt under a list in pick mode, with what's been typed so far.
    static ftxui::Element PickPrompt(const AppState* state, PickList list)
    {
        if (!IsPicking(state, list))
        {
            return ftxui::text("");
        }
        return ftxui::hbox({
            ftxui::text(RowPickPrompt(state) + "  ") | ftxui::bold | ftxui::color(kColorLabel),
            ftxui::text("#" + state->row_pick_digits + "_") | ftxui::bold |
                ftxui::color(kColorData),
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
            rows.push_back(ftxui::text(state_->row_delete_title) | ftxui::bold |
                           ftxui::color(kColorDanger));
            rows.push_back(DialogSeparator());
            for (std::size_t i = 0; i < state_->row_delete_lines.size(); ++i)
            {
                ftxui::Element line = ftxui::paragraph(state_->row_delete_lines[i]);
                rows.push_back(i == 0 ? line | ftxui::bold | ftxui::color(kColorLabel)
                                      : line | ftxui::color(kColorHint));
            }
            rows.push_back(DialogSeparator());
            rows.push_back(
                KeyHintRow({{"F2/Enter", "Yes, " + RowPickVerbFor(state_->row_delete_action)},
                            {"Esc", "Cancel"}}));
            // Long lines wrap rather than stretching the box across the
            // whole screen.
            return ftxui::vbox(rows) | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 64) |
                   ftxui::color(kColorDanger) | ftxui::borderStyled(kColorDialogBorder);
        }

    private:
        AppState* state_;
    };

    // Wraps `page` so the numbered-delete confirmation can pop up over it.
    static ftxui::Component WithRowDeleteConfirm(AppState* state, ftxui::Component page)
    {
        ftxui::Component modal =
            ftxui::Renderer(ftxui::Container::Vertical({}), RowDeleteConfirmModalRenderer(state));
        return ftxui::Modal(std::move(page), modal, &state->show_row_delete_confirm_modal);
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
                rows.push_back(i == 0 ? line | ftxui::bold | ftxui::color(kColorLabel)
                                      : line | ftxui::color(kColorHint));
            }
            rows.push_back(DialogSeparator());
            if (state_->confirm_prompt == ConfirmPrompt::kResumeNet)
            {
                rows.push_back(KeyHintRow(
                    {{"F2/Enter", "Resume"}, {"F3", "Close It & Start New"}, {"Esc", "Cancel"}}));
            }
            else
            {
                rows.push_back(KeyHintRow({{"F2/Enter", "Close Net"}, {"Esc", "Keep Logging"}}));
            }
            return ftxui::vbox(rows) | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 64) |
                   ftxui::color(kColorHeading) | ftxui::borderStyled(kColorDialogBorder);
        }

    private:
        AppState* state_;
    };

    // Wraps `page` so a ConfirmPrompt can pop up over it.
    static ftxui::Component WithConfirmPrompt(AppState* state, ftxui::Component page)
    {
        ftxui::Component modal =
            ftxui::Renderer(ftxui::Container::Vertical({}), ConfirmPromptRenderer(state));
        return ftxui::Modal(std::move(page), modal, &state->show_confirm_prompt);
    }

    // ---- Net list page ---------------------------------------------------

    class NetListRenderer
    {
    public:
        NetListRenderer(AppState* state, ftxui::Component net_menu)
            : state_(state), net_menu_(std::move(net_menu))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element net_list_elem =
                state_->nets.empty() ? HintText("No recurring nets yet. Press F2 to create one.")
                                     : PickableRows(state_, PickList::kNets, state_->net_names,
                                                    state_->selected_net_index, net_menu_);

            ftxui::Element callsign_hint =
                state_->settings.callsign.empty()
                    ? ftxui::text("No callsign set -- see Settings (F4)") |
                          ftxui::color(kColorLabel)
                    : ftxui::hbox({ftxui::text("Operating as ") | ftxui::color(kColorLabel),
                                   ftxui::text(state_->settings.callsign) | ftxui::bold |
                                       ftxui::color(kColorData)});

            ftxui::Element content = ftxui::vbox({
                callsign_hint,
                Separator(),
                Heading("Recurring Nets"),
                Framed(net_list_elem) | ftxui::flex,
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
            return PageChrome("Recurring Nets", content,
                              {
                                  {"F2", "New"},
                                  {"F3/Enter", "Start"},
                                  {"F4", "Settings"},
                                  {"F5", "AdHoc"},
                                  {"F6", "History"},
                                  {"F7", "Edit"},
                                  {"F8", "Export"},
                                  {"F9", "Import"},
                                  {"F10", "Quit"},
                              });
        }

    private:
        AppState* state_;
        ftxui::Component net_menu_;
    };

    ftxui::Component BuildNetListPage(AppState* state)
    {
        ftxui::MenuOption net_menu_option;
        net_menu_option.on_enter = StartSelectedNetHandler(state);
        net_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component net_menu =
            ftxui::Menu(&state->net_names, &state->selected_net_index, net_menu_option);

        ftxui::Component root = ftxui::Container::Vertical({net_menu});
        ftxui::Component main_view = ftxui::Renderer(root, NetListRenderer(state, net_menu));
        return WithConfirmPrompt(state, ftxui::Modal(main_view, BuildZmodemConfirmModal(state),
                                                     &state->show_zmodem_confirm_modal));
    }

    // ---- Create-net page ---------------------------------------------------

    class CreateNetRenderer
    {
    public:
        CreateNetRenderer(AppState* state, ftxui::Component input_name, ftxui::Component input_mode,
                          ftxui::Component input_frequency, ftxui::Component input_location,
                          ftxui::Component input_recurrence)
            : state_(state),
              input_name_(std::move(input_name)),
              input_mode_(std::move(input_mode)),
              input_frequency_(std::move(input_frequency)),
              input_location_(std::move(input_location)),
              input_recurrence_(std::move(input_recurrence))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element content = ftxui::vbox({
                ftxui::hbox({FieldLabel("Name:       "), input_name_->Render()}),
                ftxui::hbox({FieldLabel("Mode:       "), input_mode_->Render()}),
                ftxui::hbox({FieldLabel("Frequency:  "), input_frequency_->Render()}),
                ftxui::hbox({FieldLabel("Location:   "), input_location_->Render()}),
                ftxui::hbox({FieldLabel("Recurrence: "), input_recurrence_->Render()}),
                ErrorLine(state_->form_error),
            });

            return PageChrome("New Recurring Net", content, {{"F2", "Save"}, {"Esc", "Cancel"}});
        }

    private:
        AppState* state_;
        ftxui::Component input_name_;
        ftxui::Component input_mode_;
        ftxui::Component input_frequency_;
        ftxui::Component input_location_;
        ftxui::Component input_recurrence_;
    };

    ftxui::Component BuildCreateNetPage(AppState* state)
    {
        ftxui::Component input_name =
            ftxui::Input(&state->new_net_name, "e.g. Weekly Skywarn Net", SingleLineInputOption());
        ftxui::Component input_mode =
            ftxui::Input(&state->new_net_mode, "e.g. FM, SSB, Digital", SingleLineInputOption());
        ftxui::Component input_frequency =
            ftxui::Input(&state->new_net_frequency, "e.g. 146.940", SingleLineInputOption());
        ftxui::Component input_location =
            ftxui::Input(&state->new_net_location, "City, county, or ZIP", SingleLineInputOption());
        ftxui::Component input_recurrence = ftxui::Input(
            &state->new_net_recurrence, "e.g. Tuesdays 8pm ET", SingleLineInputOption());

        ftxui::Component root = ftxui::Container::Vertical({
            input_name,
            input_mode,
            input_frequency,
            input_location,
            input_recurrence,
        });

        state->new_net_name_input = input_name;

        return ftxui::Renderer(
            root, CreateNetRenderer(state, input_name, input_mode, input_frequency, input_location,
                                    input_recurrence));
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
            std::string net_name;
            if (state_->selected_net_index < static_cast<int>(state_->nets.size()))
            {
                net_name = state_->nets[state_->selected_net_index].name;
            }

            ftxui::Element content = ftxui::vbox({
                ftxui::hbox({ftxui::text("Starting: ") | ftxui::color(kColorLabel),
                             ftxui::text(net_name) | ftxui::bold | ftxui::color(kColorData)}),
                Separator(),
                HintText("Select your role for this net:"),
                role_radiobox_->Render(),
            });

            return PageChrome("Select Your Role", content,
                              {{"F2/Enter", "Continue"}, {"Esc", "Back"}});
        }

    private:
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

            return PageChrome("Enter Callsign", content,
                              {{"F2/Enter", "Start Net"}, {"Esc", "Back"}});
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
            const std::string& role_label = state_->role_labels[state_->selected_role_index];

            ftxui::Element check_in_list =
                state_->active_display_rows.empty()
                    ? HintText("No check-ins yet.")
                    : PickableRows(state_, PickList::kActiveCheckIns, state_->active_display_rows,
                                   state_->selected_check_in_index, check_in_menu_) |
                          ftxui::frame | ftxui::vscroll_indicator;

            // The date the net was started, with the time of day next to it.
            std::string started = state_->active_instance.instance_date;
            std::string start_time = FormatLocalTimeOfDay(state_->active_instance.started_at);
            if (!start_time.empty())
            {
                started += "  " + start_time;
            }

            ftxui::Element info_line = ftxui::hbox({
                ftxui::text("Date: ") | ftxui::color(kColorLabel),
                ftxui::text(started) | ftxui::color(kColorHeading),
                ftxui::text("   Role: ") | ftxui::color(kColorLabel),
                ftxui::text(role_label) | ftxui::color(kColorData),
                ftxui::text("   Callsign: ") | ftxui::color(kColorLabel),
                ftxui::text(state_->operator_callsign) | ftxui::bold | ftxui::color(kColorData),
            });

            ftxui::Element content = ftxui::vbox({
                info_line,
                Separator(),
                Framed(ftxui::vbox({
                    ColumnHeader(FormatCheckInHeaderRow(/*above_menu=*/true)),
                    check_in_list,
                })) |
                    ftxui::flex,
                IsPicking(state_, PickList::kActiveCheckIns)
                    ? PickPrompt(state_, PickList::kActiveCheckIns)
                    : (state_->show_new_station_modal || state_->show_edit_checkin_modal
                           ? ftxui::text("")
                           : HintText("F3 edits and F5 deletes a check-in by its #; Enter edits "
                                      "the highlighted one.")),
                StatusLine(state_->status_message),
                ErrorLine(state_->form_error),
            });

            std::string page_title =
                state_->active_net_name.empty() ? "Active Net" : state_->active_net_name;
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
                hints = {{"F2", "Save"}, {"Esc", "Cancel"}};
            }
            else if (state_->show_new_station_modal)
            {
                hints = {{"F2", "Log & Continue"}, {"F3", "Log & Close"}, {"Esc", "Cancel"}};
            }
            else
            {
                hints = {
                    {"F2", "New Check-In"}, {"F3", "Edit Check-In"}, {"F4", "Close/Save"},
                    {"F5", "Del Check-In"}, {"F7", "Export"},
                };
            }
            return PageChrome(page_title, content, hints);
        }

    private:
        AppState* state_;
        ftxui::Component check_in_menu_;
    };

    // The New Station modal, shown on top of the active-net page.
    class NewStationModalRenderer
    {
    public:
        NewStationModalRenderer(AppState* state, StationFieldInputs inputs,
                                ftxui::Component suggestion_menu,
                                ftxui::Component input_signal_report,
                                ftxui::Component input_remarks, ftxui::Component input_comment,
                                ftxui::Component role_choice_menu)
            : state_(state),
              inputs_(std::move(inputs)),
              suggestion_menu_(std::move(suggestion_menu)),
              input_signal_report_(std::move(input_signal_report)),
              input_remarks_(std::move(input_remarks)),
              input_comment_(std::move(input_comment)),
              role_choice_menu_(std::move(role_choice_menu))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element suggestions =
                state_->modal_callsign_suggestions.empty()
                    ? ftxui::text("")
                    : ftxui::vbox({
                          HintText("Matches: Up/Down to choose, Enter to pick the one marked >"),
                          DialogFramed(ftxui::vbox({
                              ColumnHeader(FormatCallsignSuggestionHeaderRow()),
                              suggestion_menu_->Render(),
                          })),
                      });

            ftxui::Elements rows;
            rows.push_back(Heading("Log Station Check-In"));
            rows.push_back(DialogSeparator());
            ftxui::Elements field_rows = StationFieldRows(inputs_);
            rows.push_back(field_rows[0]);  // Callsign
            rows.push_back(suggestions);
            for (std::size_t i = 1; i < field_rows.size(); ++i)
            {
                rows.push_back(field_rows[i]);
            }
            rows.push_back(
                ftxui::hbox({FieldLabel("Signal Report: "), input_signal_report_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Remarks:       "), input_remarks_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Comment:       "), input_comment_->Render()}));
            rows.push_back(FieldLabel("Additional Role (optional):"));
            rows.push_back(role_choice_menu_->Render());
            rows.push_back(DialogSeparator());
            rows.push_back(
                KeyHintRow({{"F2", "Log & Continue"}, {"F3", "Log & Close"}, {"Esc", "Cancel"}}));
            rows.push_back(ErrorLine(state_->form_error));

            return ftxui::vbox(rows) | ftxui::color(kColorHeading) |
                   ftxui::borderStyled(kColorDialogBorder);
        }

    private:
        AppState* state_;
        StationFieldInputs inputs_;
        ftxui::Component suggestion_menu_;
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
        EditCheckInModalRenderer(AppState* state, StationFieldInputs inputs,
                                 ftxui::Component input_signal_report,
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
            rows.push_back(ftxui::hbox({FieldLabel("Callsign:      "),
                                        ftxui::text(state_->edit_checkin_original.callsign)}));
            for (const ftxui::Element& row : StationFieldRows(inputs_))
            {
                rows.push_back(row);
            }
            rows.push_back(
                ftxui::hbox({FieldLabel("Signal Report: "), input_signal_report_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Remarks:       "), input_remarks_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Comment:       "), input_comment_->Render()}));
            rows.push_back(FieldLabel("Additional Role (optional):"));
            rows.push_back(role_choice_menu_->Render());
            rows.push_back(DialogSeparator());
            rows.push_back(KeyHintRow({{"F2", "Save"}, {"Esc", "Cancel"}}));

            return ftxui::vbox(rows) | ftxui::color(kColorHeading) |
                   ftxui::borderStyled(kColorDialogBorder);
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
        check_in_menu_option.on_enter = EditSelectedCheckInHandler(state);
        check_in_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component check_in_menu = ftxui::Menu(
            &state->active_display_rows, &state->selected_check_in_index, check_in_menu_option);

        ftxui::Component main_root = ftxui::Container::Vertical({check_in_menu});
        ftxui::Component main_view =
            ftxui::Renderer(main_root, ActiveNetRenderer(state, check_in_menu));

        ftxui::InputOption callsign_option = SingleLineInputOption();
        callsign_option.on_enter = CallsignLookupHandler(state);
        callsign_option.on_change = CallsignSuggestHandler(state);
        ftxui::Component input_callsign =
            ftxui::Input(&state->modal_station.callsign, "Callsign", callsign_option);
        StationFieldInputs modal_inputs =
            BuildStationFieldInputs(&state->modal_station, input_callsign);

        ftxui::MenuOption suggestion_menu_option;
        suggestion_menu_option.on_enter = SelectCallsignSuggestionHandler(state);
        suggestion_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component suggestion_menu =
            ftxui::Menu(&state->modal_callsign_suggestion_labels, &state->selected_suggestion_index,
                        suggestion_menu_option);

        ftxui::Component input_signal_report =
            ftxui::Input(&state->modal_signal_report, "Signal Report", SingleLineInputOption());
        ftxui::Component input_remarks =
            ftxui::Input(&state->modal_remarks, "Remarks", SingleLineInputOption());
        ftxui::Component input_comment =
            ftxui::Input(&state->modal_comment, "Comment", SingleLineInputOption());
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
            ftxui::Menu(&state->modal_role_choice_labels, &state->modal_role_choice_index,
                        role_choice_menu_option);

        state->modal_callsign_input = input_callsign;

        ftxui::Components modal_components = StationFieldComponents(modal_inputs);
        modal_components.push_back(suggestion_menu);
        modal_components.push_back(input_signal_report);
        modal_components.push_back(input_remarks);
        modal_components.push_back(input_comment);
        modal_components.push_back(role_choice_menu);
        ftxui::Component modal_root = ftxui::Container::Vertical(modal_components);
        ftxui::Component modal_view = ftxui::Renderer(
            modal_root,
            NewStationModalRenderer(state, modal_inputs, suggestion_menu, input_signal_report,
                                    input_remarks, input_comment, role_choice_menu));

        StationFieldInputs edit_checkin_inputs =
            BuildStationFieldInputs(&state->edit_checkin_station, ftxui::Component());
        ftxui::Component edit_input_signal_report = ftxui::Input(
            &state->edit_checkin_signal_report, "Signal Report", SingleLineInputOption());
        ftxui::Component edit_input_remarks =
            ftxui::Input(&state->edit_checkin_remarks, "Remarks", SingleLineInputOption());
        ftxui::Component edit_input_comment =
            ftxui::Input(&state->edit_checkin_comment, "Comment", SingleLineInputOption());
        ftxui::MenuOption edit_role_choice_menu_option;
        edit_role_choice_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component edit_role_choice_menu =
            ftxui::Menu(&state->edit_checkin_role_choice_labels,
                        &state->edit_checkin_role_choice_index, edit_role_choice_menu_option);

        ftxui::Components edit_modal_components = StationFieldComponents(edit_checkin_inputs);
        edit_modal_components.push_back(edit_input_signal_report);
        edit_modal_components.push_back(edit_input_remarks);
        edit_modal_components.push_back(edit_input_comment);
        edit_modal_components.push_back(edit_role_choice_menu);
        ftxui::Component edit_modal_root = ftxui::Container::Vertical(edit_modal_components);
        ftxui::Component edit_modal_view = ftxui::Renderer(
            edit_modal_root, EditCheckInModalRenderer(state, edit_checkin_inputs,
                                                      edit_input_signal_report, edit_input_remarks,
                                                      edit_input_comment, edit_role_choice_menu));

        ftxui::Component with_new_station_modal =
            ftxui::Modal(main_view, modal_view, &state->show_new_station_modal);
        ftxui::Component with_edit_checkin_modal =
            ftxui::Modal(with_new_station_modal, edit_modal_view, &state->show_edit_checkin_modal);
        return WithConfirmPrompt(
            state, WithRowDeleteConfirm(
                       state, ftxui::Modal(with_edit_checkin_modal, BuildZmodemConfirmModal(state),
                                           &state->show_zmodem_confirm_modal)));
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

    // ---- Settings page ---------------------------------------------------

    class SettingsRenderer
    {
    public:
        SettingsRenderer(AppState* state, ftxui::Component input_callsign,
                         ftxui::Component input_location, ftxui::Component time_format_toggle)
            : state_(state),
              input_callsign_(std::move(input_callsign)),
              input_location_(std::move(input_location)),
              time_format_toggle_(std::move(time_format_toggle))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element content = ftxui::vbox({
                ftxui::hbox({FieldLabel("My Callsign*: "), input_callsign_->Render()}),
                ftxui::hbox({FieldLabel("My ZIP Code*: "), input_location_->Render()}),
                ftxui::hbox({FieldLabel("Time Format:  "), time_format_toggle_->Render()}),
                HintText("* Required"),
                Separator(),
                HintParagraph("My ZIP Code is a plain 5-digit US ZIP code (digits only), used "
                              "to find nearby licensed stations when saving a station to a "
                              "net. Never included when the database is exported."),
                HintParagraph("Time Format (Left/Right to change) sets how every time is shown "
                              "and exported. Times are shown in the time zone of the computer "
                              "QuickLogger runs on."),
                Separator(),
                Heading("Station data (shared by everyone, kept up to date automatically):"),
                ftxui::paragraph(DescribeStationDataStatus(
                    state_->db, static_cast<std::int64_t>(std::time(nullptr)),
                    state_->is_console_session)) |
                    ftxui::color(kColorHeading),
                StatusLine(state_->status_message),
                ErrorLine(state_->form_error),
            });

            std::vector<KeyHint> hints = {{"F2", "Save"}};
            if (state_->is_console_session)
            {
                hints.push_back({"F3", "Refresh Station Data"});
                hints.push_back({"F4", "Manage Users"});
            }
            hints.push_back({"Esc", "Cancel"});
            return PageChrome("Settings", content, hints);
        }

    private:
        AppState* state_;
        ftxui::Component input_callsign_;
        ftxui::Component input_location_;
        ftxui::Component time_format_toggle_;
    };

    ftxui::Component BuildSettingsPage(AppState* state)
    {
        ftxui::InputOption callsign_option = SingleLineInputOption();
        callsign_option.on_change = UppercaseFieldHandler(&state->settings_form.callsign);
        ftxui::Component input_callsign =
            ftxui::Input(&state->settings_form.callsign, "Your callsign", callsign_option);
        ftxui::InputOption location_option = SingleLineInputOption();
        location_option.on_change = ZipCodeFieldHandler(&state->settings_form.location);
        ftxui::Component input_location =
            ftxui::Input(&state->settings_form.location, "5-digit ZIP", location_option);

        ftxui::MenuOption time_format_option = ftxui::MenuOption::Toggle();
        time_format_option.entries_option.transform = ToggleEntryTransform;
        time_format_option.elements_infix = ToggleGap;
        time_format_option.focused_entry = &state->settings_time_format_index;
        ftxui::Component time_format_toggle = std::make_shared<IgnoreTab>(
            ftxui::Menu(&state->settings_time_format_labels, &state->settings_time_format_index,
                        time_format_option));

        ftxui::Component root = ftxui::Container::Vertical({
            input_callsign,
            input_location,
            time_format_toggle,
        });

        return ftxui::Renderer(
            root, SettingsRenderer(state, input_callsign, input_location, time_format_toggle));
    }

    // ---- Ad hoc net page ---------------------------------------------------

    class AdHocNetRenderer
    {
    public:
        AdHocNetRenderer(AppState* state, ftxui::Component input_name, ftxui::Component input_mode,
                         ftxui::Component input_frequency, ftxui::Component input_location)
            : state_(state),
              input_name_(std::move(input_name)),
              input_mode_(std::move(input_mode)),
              input_frequency_(std::move(input_frequency)),
              input_location_(std::move(input_location))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element content = ftxui::vbox({
                HintText("Logs a one-off net without saving it as a recurring net."),
                Separator(),
                ftxui::hbox({FieldLabel("Name:      "), input_name_->Render()}),
                ftxui::hbox({FieldLabel("Mode:      "), input_mode_->Render()}),
                ftxui::hbox({FieldLabel("Frequency: "), input_frequency_->Render()}),
                ftxui::hbox({FieldLabel("Location:  "), input_location_->Render()}),
                ErrorLine(state_->form_error),
            });

            return PageChrome("Ad Hoc Net", content, {{"F2", "Start Net"}, {"Esc", "Cancel"}});
        }

    private:
        AppState* state_;
        ftxui::Component input_name_;
        ftxui::Component input_mode_;
        ftxui::Component input_frequency_;
        ftxui::Component input_location_;
    };

    ftxui::Component BuildAdHocNetPage(AppState* state)
    {
        ftxui::Component input_name =
            ftxui::Input(&state->new_net_name, "e.g. Tailgate Net", SingleLineInputOption());
        ftxui::Component input_mode =
            ftxui::Input(&state->new_net_mode, "e.g. FM, SSB, Digital", SingleLineInputOption());
        ftxui::Component input_frequency =
            ftxui::Input(&state->new_net_frequency, "e.g. 146.940", SingleLineInputOption());
        ftxui::Component input_location =
            ftxui::Input(&state->new_net_location, "City, county, or ZIP", SingleLineInputOption());

        ftxui::Component root = ftxui::Container::Vertical({
            input_name,
            input_mode,
            input_frequency,
            input_location,
        });

        state->ad_hoc_net_name_input = input_name;

        return ftxui::Renderer(
            root, AdHocNetRenderer(state, input_name, input_mode, input_frequency, input_location));
    }

    // ---- Net history page ---------------------------------------------------

    class NetHistoryRenderer
    {
    public:
        NetHistoryRenderer(AppState* state, ftxui::Component instance_menu,
                           ftxui::Component checkin_menu)
            : state_(state),
              instance_menu_(std::move(instance_menu)),
              checkin_menu_(std::move(checkin_menu))
        {
        }

        ftxui::Element operator()() const
        {
            std::string net_name;
            if (state_->selected_net_index < static_cast<int>(state_->nets.size()))
            {
                net_name = state_->nets[state_->selected_net_index].name;
            }

            ftxui::Element instance_list =
                state_->history_instances.empty()
                    ? HintText("No past instances of this net yet.")
                    : PickableRows(state_, PickList::kNetInstances, state_->history_instance_labels,
                                   state_->selected_history_index, instance_menu_) |
                          ftxui::frame | ftxui::vscroll_indicator;

            ftxui::Element checkin_list =
                state_->history_check_in_labels.empty()
                    ? HintText(state_->history_instances.empty() ? "" : "No check-ins were logged.")
                    : PickableRows(state_, PickList::kHistoryCheckIns,
                                   state_->history_check_in_labels,
                                   state_->selected_history_check_in_index, checkin_menu_) |
                          ftxui::frame | ftxui::vscroll_indicator;

            ftxui::Element content = ftxui::vbox({
                Framed(ftxui::vbox({
                    ColumnHeader(PickHeaderPad(state_, PickList::kNetInstances) +
                                 FormatNetInstanceHeaderRow()),
                    instance_list,
                })) |
                    ftxui::flex,
                PickPrompt(state_, PickList::kNetInstances),
                Separator(),
                Framed(ftxui::vbox({
                    ColumnHeader(FormatCheckInHeaderRow(/*above_menu=*/true)),
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
            return PageChrome("History: " + net_name, content,
                              {{"F4", "Delete Check-In"},
                               {"F5", "Delete Session"},
                               {"F7", "Export"},
                               {"Esc", "Back"}});
        }

    private:
        AppState* state_;
        ftxui::Component instance_menu_;
        ftxui::Component checkin_menu_;
    };

    ftxui::Component BuildNetHistoryPage(AppState* state)
    {
        ftxui::MenuOption instance_menu_option;
        instance_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        instance_menu_option.on_change = HistoryInstanceChangedHandler(state);
        ftxui::Component instance_menu = ftxui::Menu(
            &state->history_instance_labels, &state->selected_history_index, instance_menu_option);

        ftxui::MenuOption checkin_menu_option;
        checkin_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component checkin_menu =
            ftxui::Menu(&state->history_check_in_labels, &state->selected_history_check_in_index,
                        checkin_menu_option);

        ftxui::Component root = ftxui::Container::Vertical({instance_menu, checkin_menu});
        ftxui::Component main_view =
            ftxui::Renderer(root, NetHistoryRenderer(state, instance_menu, checkin_menu));
        return WithRowDeleteConfirm(state, ftxui::Modal(main_view, BuildZmodemConfirmModal(state),
                                                        &state->show_zmodem_confirm_modal));
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
            return ftxui::vbox(rows) | ftxui::color(kColorDanger) |
                   ftxui::borderStyled(kColorDialogBorder);
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
                        ftxui::Component input_frequency, ftxui::Component input_location,
                        ftxui::Component input_recurrence, ftxui::Component saved_station_menu,
                        StationFieldInputs saved_station_inputs,
                        ftxui::Component saved_station_suggestion_menu,
                        ftxui::Component saved_station_remarks_input)
            : state_(state),
              input_name_(std::move(input_name)),
              input_mode_(std::move(input_mode)),
              input_frequency_(std::move(input_frequency)),
              input_location_(std::move(input_location)),
              input_recurrence_(std::move(input_recurrence)),
              saved_station_menu_(std::move(saved_station_menu)),
              saved_station_inputs_(std::move(saved_station_inputs)),
              saved_station_suggestion_menu_(std::move(saved_station_suggestion_menu)),
              saved_station_remarks_input_(std::move(saved_station_remarks_input))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element saved_station_list =
                state_->edit_net_saved_stations.empty()
                    ? HintText("No saved stations yet.")
                    : PickableRows(state_, PickList::kSavedStations,
                                   state_->edit_net_saved_station_labels,
                                   state_->selected_saved_station_index, saved_station_menu_) |
                          ftxui::frame | ftxui::vscroll_indicator;

            ftxui::Element suggestions =
                state_->saved_station_suggestions.empty()
                    ? ftxui::text("")
                    : ftxui::vbox({
                          HintText("Matches: Up/Down to choose, Enter to pick the one marked >"),
                          Framed(ftxui::vbox({
                              ColumnHeader(FormatCallsignSuggestionHeaderRow()),
                              saved_station_suggestion_menu_->Render(),
                          })),
                      });

            ftxui::Elements rows;
            rows.push_back(ftxui::hbox({FieldLabel("Name:       "), input_name_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Mode:       "), input_mode_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Frequency:  "), input_frequency_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Location:   "), input_location_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Recurrence: "), input_recurrence_->Render()}));
            rows.push_back(Separator());
            rows.push_back(Heading("Saved Stations:"));
            rows.push_back(Framed(ftxui::vbox({
                               ColumnHeader(PickHeaderPad(state_, PickList::kSavedStations) +
                                            FormatSavedStationHeaderRow(/*above_menu=*/true)),
                               saved_station_list,
                           })) |
                           ftxui::flex);
            if (IsPicking(state_, PickList::kSavedStations))
            {
                rows.push_back(PickPrompt(state_, PickList::kSavedStations));
            }
            else
            {
                rows.push_back(
                    HintText("F9 (or Enter on a highlighted station) loads a station into the "
                             "fields below; F4 removes one."));
            }
            rows.push_back(Separator());
            ftxui::Elements field_rows = StationFieldRows(saved_station_inputs_);
            rows.push_back(field_rows[0]);  // Callsign
            rows.push_back(suggestions);
            for (std::size_t i = 1; i < field_rows.size(); ++i)
            {
                rows.push_back(field_rows[i]);
            }
            rows.push_back(ftxui::hbox(
                {FieldLabel("Default Remarks: "), saved_station_remarks_input_->Render()}));
            rows.push_back(StatusLine(state_->status_message));
            rows.push_back(ErrorLine(state_->form_error));

            if (state_->row_pick_action != RowPickAction::kNone)
            {
                return PageChrome("Edit Net: " + state_->edit_net_name, ftxui::vbox(rows),
                                  PickKeyHints(state_));
            }
            return PageChrome("Edit Net: " + state_->edit_net_name, ftxui::vbox(rows),
                              {
                                  {"F2", "Save Net"},
                                  {"F3", "Save Station"},
                                  {"F4", "Remove"},
                                  {"F6", "Add Station"},
                                  {"F7", "Export"},
                                  {"F8", "Delete Net"},
                                  {"F9", "Edit Station"},
                                  {"Esc", "Cancel"},
                              });
        }

    private:
        AppState* state_;
        ftxui::Component input_name_;
        ftxui::Component input_mode_;
        ftxui::Component input_frequency_;
        ftxui::Component input_location_;
        ftxui::Component input_recurrence_;
        ftxui::Component saved_station_menu_;
        StationFieldInputs saved_station_inputs_;
        ftxui::Component saved_station_suggestion_menu_;
        ftxui::Component saved_station_remarks_input_;
    };

    ftxui::Component BuildEditNetPage(AppState* state)
    {
        ftxui::Component input_name =
            ftxui::Input(&state->edit_net_name, "e.g. Weekly Skywarn Net", SingleLineInputOption());
        ftxui::Component input_mode =
            ftxui::Input(&state->edit_net_mode, "e.g. FM, SSB, Digital", SingleLineInputOption());
        ftxui::Component input_frequency =
            ftxui::Input(&state->edit_net_frequency, "e.g. 146.940", SingleLineInputOption());
        ftxui::Component input_location = ftxui::Input(
            &state->edit_net_location, "City, county, or ZIP", SingleLineInputOption());
        ftxui::Component input_recurrence = ftxui::Input(
            &state->edit_net_recurrence, "e.g. Tuesdays 8pm ET", SingleLineInputOption());

        ftxui::MenuOption saved_station_menu_option;
        saved_station_menu_option.on_enter = LoadSavedStationHandler(state);
        saved_station_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component saved_station_menu =
            ftxui::Menu(&state->edit_net_saved_station_labels, &state->selected_saved_station_index,
                        saved_station_menu_option);

        ftxui::InputOption saved_station_callsign_option = SingleLineInputOption();
        saved_station_callsign_option.on_change = SavedStationCallsignChangeHandler(state);
        saved_station_callsign_option.on_enter = SavedStationCallsignEnterHandler(state);
        ftxui::Component saved_station_callsign_input =
            ftxui::Input(&state->saved_station.callsign, "Callsign", saved_station_callsign_option);
        state->saved_station_callsign_input = saved_station_callsign_input;
        StationFieldInputs saved_station_inputs =
            BuildStationFieldInputs(&state->saved_station, saved_station_callsign_input);
        ftxui::Component saved_station_remarks_input =
            ftxui::Input(&state->saved_station_remarks, "Optional", SingleLineInputOption());

        ftxui::MenuOption saved_station_suggestion_menu_option;
        saved_station_suggestion_menu_option.on_enter = SelectSavedStationSuggestionHandler(state);
        saved_station_suggestion_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component saved_station_suggestion_menu = ftxui::Menu(
            &state->saved_station_suggestion_labels,
            &state->selected_saved_station_suggestion_index, saved_station_suggestion_menu_option);

        ftxui::Components saved_station_field_components =
            StationFieldComponents(saved_station_inputs);
        saved_station_field_components.insert(saved_station_field_components.begin() + 1,
                                              saved_station_suggestion_menu);
        saved_station_field_components.push_back(saved_station_remarks_input);

        ftxui::Components root_components = {
            input_name,     input_mode,       input_frequency,
            input_location, input_recurrence, saved_station_menu,
        };
        for (const ftxui::Component& component : saved_station_field_components)
        {
            root_components.push_back(component);
        }

        ftxui::Component root = ftxui::Container::Vertical(root_components);

        state->edit_net_name_input = input_name;

        ftxui::Component main_view = ftxui::Renderer(
            root, EditNetRenderer(state, input_name, input_mode, input_frequency, input_location,
                                  input_recurrence, saved_station_menu, saved_station_inputs,
                                  saved_station_suggestion_menu, saved_station_remarks_input));
        ftxui::Component with_zmodem_modal = ftxui::Modal(main_view, BuildZmodemConfirmModal(state),
                                                          &state->show_zmodem_confirm_modal);
        return WithRowDeleteConfirm(
            state, ftxui::Modal(with_zmodem_modal, BuildDeleteNetConfirmModal(state),
                                &state->show_delete_net_confirm_modal));
    }

    // ---- Import net page ---------------------------------------------------

    class ImportNetRenderer
    {
    public:
        ImportNetRenderer(AppState* state, ftxui::Component file_menu)
            : state_(state), file_menu_(std::move(file_menu))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element file_list =
                state_->import_net_files.empty()
                    ? HintText(
                          "No *.qlnet files in imports/ yet. Press F3 to receive one via "
                          "ZMODEM.")
                    : file_menu_->Render() | ftxui::frame | ftxui::vscroll_indicator;

            ftxui::Element content = ftxui::vbox({
                Heading("Files ready to import:"),
                Framed(file_list) | ftxui::flex,
                StatusLine(state_->status_message),
                ErrorLine(state_->form_error),
            });

            return PageChrome(
                "Import Net", content,
                {{"F2/Enter", "Import"}, {"F3", "Receive (ZMODEM)"}, {"Esc", "Back"}});
        }

    private:
        AppState* state_;
        ftxui::Component file_menu_;
    };

    ftxui::Component BuildImportNetPage(AppState* state)
    {
        ftxui::MenuOption file_menu_option;
        file_menu_option.on_enter = ImportSelectedNetSliceHandler(state);
        file_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component file_menu = ftxui::Menu(
            &state->import_net_files, &state->selected_import_file_index, file_menu_option);

        ftxui::Component root = ftxui::Container::Vertical({file_menu});
        ftxui::Component main_view = ftxui::Renderer(root, ImportNetRenderer(state, file_menu));
        return ftxui::Modal(main_view, BuildZmodemConfirmModal(state),
                            &state->show_zmodem_confirm_modal);
    }

    // ---- Manage users page --------------------------------------------------

    // Console-only (see AppState::is_console_session) -- reached from
    // Settings' F4, never over SSH. Lists who's allowed to SSH in and lets
    // the console operator add/remove entries; there's no admin/permission
    // concept to check here precisely because an SSH session can never
    // reach this page at all, regardless of whose key it authenticated
    // with (see ShowManageUsersPageHandler's doc comment).
    class ManageUsersRenderer
    {
    public:
        ManageUsersRenderer(AppState* state, ftxui::Component user_menu,
                            ftxui::Component input_username, ftxui::Component input_public_key)
            : state_(state),
              user_menu_(std::move(user_menu)),
              input_username_(std::move(input_username)),
              input_public_key_(std::move(input_public_key))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element user_list =
                state_->manage_users.empty()
                    ? HintText("No SSH users yet.")
                    : PickableRows(state_, PickList::kUsers, state_->manage_users_labels,
                                   state_->selected_user_index, user_menu_) |
                          ftxui::frame | ftxui::vscroll_indicator;

            ftxui::Element content = ftxui::vbox({
                Heading("SSH Users:"),
                Framed(user_list) | ftxui::flex,
                PickPrompt(state_, PickList::kUsers),
                Separator(),
                ftxui::hbox({FieldLabel("Username:    "), input_username_->Render()}),
                ftxui::hbox({FieldLabel("Public Key:  "), input_public_key_->Render()}),
                HintParagraph("Paste a full authorized_keys-style line, e.g. from "
                              "~/.ssh/id_ed25519.pub -- \"ssh-ed25519 AAAA... comment\"."),
                StatusLine(state_->status_message),
                ErrorLine(state_->form_error),
            });

            if (state_->row_pick_action != RowPickAction::kNone)
            {
                return PageChrome("Manage Users", content, PickKeyHints(state_));
            }
            return PageChrome("Manage Users", content,
                              {{"F2", "Add"}, {"F3", "Remove"}, {"Esc", "Back"}});
        }

    private:
        AppState* state_;
        ftxui::Component user_menu_;
        ftxui::Component input_username_;
        ftxui::Component input_public_key_;
    };

    ftxui::Component BuildManageUsersPage(AppState* state)
    {
        ftxui::MenuOption user_menu_option;
        user_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component user_menu =
            ftxui::Menu(&state->manage_users_labels, &state->selected_user_index, user_menu_option);
        ftxui::Component input_username =
            ftxui::Input(&state->new_user_username, "Username", SingleLineInputOption());
        ftxui::Component input_public_key = ftxui::Input(
            &state->new_user_public_key, "ssh-ed25519 AAAA... comment", SingleLineInputOption());

        ftxui::Component root =
            ftxui::Container::Vertical({user_menu, input_username, input_public_key});
        return WithRowDeleteConfirm(
            state, ftxui::Renderer(root, ManageUsersRenderer(state, user_menu, input_username,
                                                             input_public_key)));
    }

}  // namespace ql
