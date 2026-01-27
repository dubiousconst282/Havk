#include <doctest/doctest.h>
#include <ostream>

#include <Havx/DataIO.h>

TEST_CASE("reader binary primitives") {
    auto mem = havx::io::MemoryWriteStream();
    auto wr = havx::io::StreamWriter::CreateFromStream(&mem);
    wr.WriteF32(3.14159f);
    wr.WriteULen(123);
    wr.WriteULen(12356789);
    wr.WriteU32(0xA0B2C4D6);
    wr.WriteString("The quick brown fox jumped over the lazy dog.");
    wr.Flush();

    auto rd = havx::io::StreamReader::CreateFromMemory(mem.Buffer);
    CHECK_EQ(rd.ReadF32(), 3.14159f);
    CHECK_EQ(rd.ReadULen(), 123);
    CHECK_EQ(rd.ReadULen(), 12356789);
    CHECK_EQ(rd.ReadU32(), 0xA0B2C4D6);
    CHECK_EQ(rd.ReadString(), "The quick brown fox jumped over the lazy dog.");

    CHECK(rd.HasEnded());
}

TEST_CASE("reader lines") {
    std::string text = "The quick brown fox\njumped over the lazy dog.\n\n\nSkip empty lines\nSkip CRLF\r\nEnd without new line";
    auto rd = havx::io::StreamReader::CreateFromMemory({ (const uint8_t*)text.data(), text.size() });

    std::string ln;
    CHECK(rd.ReadLine(ln)); CHECK_EQ(ln, "The quick brown fox");
    CHECK(rd.ReadLine(ln)); CHECK_EQ(ln, "jumped over the lazy dog.");
    CHECK(rd.ReadLine(ln)); CHECK_EQ(ln, "Skip empty lines");
    CHECK(rd.ReadLine(ln)); CHECK_EQ(ln, "Skip CRLF");
    CHECK(rd.ReadLine(ln)); CHECK_EQ(ln, "End without new line");
    CHECK(!rd.ReadLine(ln));
    CHECK(rd.HasEnded());
}

TEST_CASE("reader stream and memory") {
    auto data = havx::fs::ReadBytes(__FILE__).value();
    auto fs = havx::fs::FileStream::OpenRead(__FILE__).value();

    auto rdMem = havx::io::StreamReader::CreateFromMemory(data);
    auto rdStr = havx::io::StreamReader::CreateFromStream(&fs);

    while (true) {
        if (rdMem.HasEnded() != rdStr.HasEnded()) FAIL("Mismatch between StreamReader backed by stream vs memory");
        if (rdMem.HasEnded()) break;

        uint8_t v1 = rdMem.ReadU8();
        uint8_t v2 = rdStr.ReadU8();
        if (v1 != v2) FAIL("Mismatch between StreamReader backed by stream vs memory");
    }
}

TEST_CASE("file stream and directories") {
    {
        auto deleteRes = havx::fs::DeleteDir("tests/out", true);
        CHECK_UNARY(deleteRes.has_value() || deleteRes.error() == havx::fs::Error::NotFound);

        auto fs = havx::fs::FileStream::CreateTrunc("tests/out/temp.txt").value();

        CHECK_EQ(havx::fs::CreateDirs("tests/out/").error(), havx::fs::Error::AlreadyExists);
        CHECK_UNARY(havx::fs::CreateDirs("tests/out/create/many/sub/dirs/../dirs").has_value());
        CHECK_EQ(havx::fs::CreateDirs("tests/out/create/many/sub/dirs").error(), havx::fs::Error::AlreadyExists);

        CHECK_EQ(havx::fs::FileStream::CreateNew("tests/out/temp.txt").error(), havx::fs::Error::AlreadyExists);
        CHECK_EQ(havx::fs::FileStream::OpenRead("tests/out/never.txt").error(), havx::fs::Error::NotFound);

        fs.Write("Hello world", 11);

        CHECK_EQ(fs.GetPosition(), 11);
        CHECK_EQ(fs.GetLength(), 11);

        fs.Seek(0);
        char buffer[64];
        CHECK_EQ(fs.Read(buffer, sizeof(buffer)), 11);
        CHECK_EQ(memcmp(buffer, "Hello world", 11), 0);
    }
    
    {
        auto fs = havx::fs::FileStream::OpenRead("tests/out/temp.txt").value();

        CHECK_EQ(fs.GetPosition(), 0);
        CHECK_EQ(fs.GetLength(), 11);

        char buffer[64];

        CHECK_EQ(fs.Read(buffer, sizeof(buffer)), 11);
        CHECK_EQ(memcmp(buffer, "Hello world", 11), 0);
    }
    havx::fs::DeleteFile("tests/out/temp.txt").value();
    CHECK_EQ(havx::fs::DeleteFile("tests/out/temp.txt").error(), havx::fs::Error::NotFound);
}

TEST_CASE("path manip") {
    using namespace std::literals;

    CHECK_EQ(havx::fs::GetParentPath("/usr/foo/bar/"), "/usr/foo"sv); // different behavior from std::fs and dotnet!
    CHECK_EQ(havx::fs::GetParentPath("/usr/foo/bar"), "/usr/foo"sv);
    CHECK_EQ(havx::fs::GetParentPath("/usr///bar//"), "/usr"sv);
    CHECK_EQ(havx::fs::GetParentPath("/"), ""sv);

#if _WIN32
    CHECK_EQ(havx::fs::GetParentPath("C:/"), ""sv);
    CHECK_EQ(havx::fs::GetParentPath("C:/Users"), "C:/"sv);

    CHECK_EQ(havx::fs::GetParentPath("//?/C:/Users"), "//?/C:/"sv);
    CHECK_EQ(havx::fs::GetParentPath("//?/C:/"), ""sv);
    CHECK_EQ(havx::fs::GetParentPath("//127.0.0.1/netshare/apps"), "//127.0.0.1/netshare"sv);
    CHECK_EQ(havx::fs::GetParentPath("//127.0.0.1/netshare"), "//127.0.0.1"sv);
    CHECK_EQ(havx::fs::GetParentPath("//127.0.0.1"), ""sv);
    CHECK_EQ(havx::fs::GetParentPath("//?/UNC/127.0.0.1/netshare/"), "//?/UNC/127.0.0.1"sv);
    CHECK_EQ(havx::fs::GetParentPath("//?/UNC/127.0.0.1"), ""sv);
#endif

    CHECK_EQ(havx::fs::GetFileName("/home/cats.jpg"), "cats.jpg");
    CHECK_EQ(havx::fs::GetFileName("/home/admin/"), "");

    CHECK_EQ(havx::fs::ReplaceExtension("/usr/foo.bar", ".txt"), "/usr/foo.txt");
    CHECK_EQ(havx::fs::ReplaceExtension("/usr/foo", "txt"), "/usr/foo.txt");
    CHECK_EQ(havx::fs::ReplaceExtension("/usr/", ".txt"), "/usr/.txt");
    CHECK_EQ(havx::fs::ReplaceExtension(".bar", ".txt"), ".txt");
    CHECK_EQ(havx::fs::ReplaceExtension("", ".txt"), ".txt");

    auto currDir = std::string(havx::fs::GetParentPath(__FILE__));
    CHECK_EQ(havx::fs::GetAbsolutePath(currDir + "/../tests"), currDir);
}
