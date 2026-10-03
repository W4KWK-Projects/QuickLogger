// The old SCP protocol on the SSH server (scp_server.hpp), run over a buffer
// instead of an SSH channel.

#include <cstdint>
#include <string>

#include "../src/file_export.hpp"
#include "../src/scp_server.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // What the client sends, all of it up front, and what the server wrote.
    class BufferScpChannel : public ScpChannel
    {
    public:
        explicit BufferScpChannel(std::string input) : input_(std::move(input))
        {
        }

        bool Read(char* data, std::size_t size) override
        {
            if (input_.size() - position_ < size)
            {
                position_ = input_.size();
                return false;
            }
            input_.copy(data, size, position_);
            position_ += size;
            return true;
        }

        bool Write(const char* data, std::size_t size) override
        {
            output_.append(data, size);
            return true;
        }

        const std::string& output() const
        {
            return output_;
        }

    private:
        std::string input_;
        std::size_t position_ = 0;
        std::string output_;
    };

    static std::string Nul(const std::string& text)
    {
        return text + std::string(1, '\0');
    }

    static ScpCommand ParsedScp(const std::string& line)
    {
        ScpCommand command;
        std::string error;
        REQUIRE(ParseScpCommand(line, &command, &error));
        return command;
    }

    QL_TEST(ScpCommandsParse)
    {
        CHECK(IsScpCommandLine("scp -t /imports/"));
        CHECK(IsScpCommandLine("  scp"));
        CHECK(!IsScpCommandLine("scpx -t /imports/"));
        CHECK(!IsScpCommandLine("import-session a.qlsession"));

        ScpCommand command = ParsedScp("scp -v -p -d -t -- /imports/");
        CHECK(command.receive);
        CHECK(command.preserve);
        CHECK(command.target_is_folder);
        CHECK_EQ(command.path, std::string("/imports/"));
        command = ParsedScp("scp -f /exports/Log.txt");
        CHECK(!command.receive);
        CHECK_EQ(command.path, std::string("/exports/Log.txt"));

        std::string error;
        CHECK(!ParseScpCommand("scp -r -t /imports/", &command, &error));
        CHECK(!ParseScpCommand("scp -t -f /imports/", &command, &error));
        CHECK(!ParseScpCommand("scp /imports/", &command, &error));
        CHECK(!ParseScpCommand("scp -t a b", &command, &error));
        CHECK(!ParseScpCommand("scp -x -t /imports/", &command, &error));
        CHECK(!ParseScpCommand("scp -t /imports/; ls", &command, &error));
    }

    QL_TEST(ScpFileLinesParse)
    {
        std::uint64_t size = 0;
        std::string name;
        std::string error;
        REQUIRE(ParseScpFileLine("C0644 1234 Net name.qlnet", &size, &name, &error));
        CHECK_EQ(size, std::uint64_t(1234));
        CHECK_EQ(name, std::string("Net name.qlnet"));
        CHECK(!ParseScpFileLine("C0644 12x4 Net.qlnet", &size, &name, &error));
        CHECK(!ParseScpFileLine("C0944 1 Net.qlnet", &size, &name, &error));
        CHECK(!ParseScpFileLine("C0644 1 ../Net.qlnet", &size, &name, &error));
        CHECK(!ParseScpFileLine("C0644 1 ..", &size, &name, &error));
        CHECK(!ParseScpFileLine("C0644 1 ", &size, &name, &error));
        CHECK(!ParseScpFileLine("C0644 99999999999999999999 Net.qlnet", &size, &name, &error));
    }

    QL_TEST(ScpReceivesIntoImports)
    {
        TempDir dir;
        std::string db_path = dir.File("quicklogger.db");
        BufferScpChannel channel(Nul("T1700000000 0 1700000000 0\nC0644 5 Net.qlnet\nhello") +
                                 "C0644 3 notes.txt\n" + "C0644 2 Two.qlsession\nhi" + std::string(1, '\0'));
        int status = RunScpCommand(ParsedScp("scp -p -d -t /imports/"), &channel, db_path, "W4KWK", false);
        // notes.txt is refused (and makes the copy fail), the rest arrive.
        CHECK_EQ(status, 1);
        std::string imports = SessionImportsDir(db_path, "W4KWK");
        CHECK_EQ(ReadTextFile(imports + "/Net.qlnet"), std::string("hello"));
        CHECK_EQ(ReadTextFile(imports + "/Two.qlsession"), std::string("hi"));
        CHECK(!FileExists(imports + "/notes.txt"));
        CHECK(channel.output().find("\x01scp: notes.txt: only .qlnet and .qlsession files.\n") != std::string::npos);
    }

    QL_TEST(ScpRenamesToANamedTarget)
    {
        TempDir dir;
        std::string db_path = dir.File("quicklogger.db");
        BufferScpChannel channel(Nul("C0644 2 local.qlnet\nok"));
        CHECK_EQ(RunScpCommand(ParsedScp("scp -t imports/Remote.qlnet"), &channel, db_path, "W4KWK", false), 0);
        CHECK_EQ(ReadTextFile(SessionImportsDir(db_path, "W4KWK") + "/Remote.qlnet"), std::string("ok"));
        CHECK_EQ(channel.output(), std::string(3, '\0'));
    }

    QL_TEST(ScpRefusesWhatSftpRefuses)
    {
        TempDir dir;
        std::string db_path = dir.File("quicklogger.db");
        BufferScpChannel too_big("C0644 26214401 Big.qlnet\n");
        CHECK_EQ(RunScpCommand(ParsedScp("scp -t /imports/"), &too_big, db_path, "W4KWK", false), 1);
        CHECK(too_big.output().find("over the 25 MB limit") != std::string::npos);

        BufferScpChannel view_only("");
        CHECK_EQ(RunScpCommand(ParsedScp("scp -t /imports/"), &view_only, db_path, "W4KWK", true), 1);
        CHECK(view_only.output().rfind("\x02", 0) == 0);

        BufferScpChannel exports("");
        CHECK_EQ(RunScpCommand(ParsedScp("scp -t /exports/"), &exports, db_path, "W4KWK", false), 1);
        CHECK(!FileExists(SessionImportsDir(db_path, "W4KWK") + "/Big.qlnet"));
    }

    QL_TEST(ScpSendsOneFile)
    {
        TempDir dir;
        std::string db_path = dir.File("quicklogger.db");
        std::string exports = SessionExportsDir(db_path, "W4KWK");
        REQUIRE(EnsureDirectory(exports));
        WriteTextFile(exports + "/Log.txt", "log");

        BufferScpChannel channel(std::string(3, '\0'));
        CHECK_EQ(RunScpCommand(ParsedScp("scp -f /exports/Log.txt"), &channel, db_path, "W4KWK", true), 0);
        CHECK_EQ(channel.output(), Nul("C0644 3 Log.txt\nlog"));

        BufferScpChannel missing(std::string(1, '\0'));
        CHECK_EQ(RunScpCommand(ParsedScp("scp -f /exports/None.txt"), &missing, db_path, "W4KWK", false), 1);
        CHECK(missing.output().rfind("\x01scp: None.txt: No such file.\n", 0) == 0);

        BufferScpChannel outside(std::string(1, '\0'));
        CHECK_EQ(RunScpCommand(ParsedScp("scp -f /settings/W4KWK.txt"), &outside, db_path, "W4KWK", false), 1);
    }

}  // namespace ql
