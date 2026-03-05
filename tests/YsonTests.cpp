#include <doctest/doctest.h>

#include <glm/glm.hpp>
#define YSON_ENABLE_EXTRA_GLM
#define YSON_ENABLE_EXTRA_STD_UNORDERED_MAP
#include <Havx/Yson.h>
#include <limits>

TEST_CASE("stream reader") {
    auto inputStr = "{ int: 1, \"str\": 'is this thing\\\n \\u00a2 \\' working', # can it handle meaningless comments\nfloat: 3.14159, arr: [1,2,3,{},[],5,'stars in the sky'], nest: {a:bcd, y: true, x: null}, f2: 123.5e6 }";

    auto rd = yson::Reader(inputStr);
    auto wr = yson::Writer();
    wr.IndentWidth = 0;

    auto paste = [&](auto& self) -> void {
        switch (rd.Type) {
            case yson::kTypeNumber:
            case yson::kTypeInt: {
                wr.WriteNum(rd.GetNum());
                break;
            }
            case yson::kTypeString:
            case yson::kTypeIdentifier: {
                wr.WriteStr(rd.GetString());
                break;
            }
            case yson::kTypeObject: {
                wr.BeginObject();
                while (rd.ReadNext()) {
                    wr.WriteProp(rd.Key);
                    self(self);
                }
                wr.EndObject();
                break;
            }
            case yson::kTypeArray: {
                wr.BeginArray();
                while (rd.ReadNext()) {
                    self(self);
                }
                wr.EndArray();
                break;
            }
            default: FAIL("Unexpected value type");
        }
    };

    rd.ReadNext();
    paste(paste);

    std::string expReconStr = R"({int: 1, str: "is this thing\
 ¢ ' working", float: 3.14159, arr: [1, 2, 3, {}, [], 5, "stars in the sky"], nest: {a: "bcd", y: "true", x: "null"}, f2: 123500000})";
    CHECK(wr.Buffer == expReconStr);
}

TEST_CASE("writer formatting") {
    auto write = [](uint32_t indent) {
        yson::Writer w;
        w.IndentWidth = indent;
        w.BeginObject();
        w.WriteInt("dec", 12345);
        w.WriteUInt("bin", 0x15, 2, 8);
        w.WriteUInt("hex", 456, 16, 4);
        w.WriteNum("flt", 3.14159);
        w.WriteNum("flt2", std::numeric_limits<double>::quiet_NaN());
        w.WriteNum("flt3", std::numeric_limits<double>::infinity());

        w.WriteStr("str", "hello world! does escaping work? quotes \" and control \r\n\tEmoji: 🙌.");
        w.BeginObject("nested_obj");
        {
            w.BeginObject("thing");
            w.IndentWidth = 0;
            w.WriteInt("ver", 987);
            w.WriteInt("rev", 789);
            w.EndObject();
            w.IndentWidth = indent;

            w.BeginObject("empty_obj");
            w.EndObject();

            w.BeginArray("coords");
            w.IndentWidth = 0;
            w.WriteInt(123);
            w.WriteInt(456);
            w.WriteInt(789);
            w.EndArray();
            w.IndentWidth = indent;

            w.WriteInt("end", 1);
        }
        w.EndObject();

        w.BeginArray("nest_array");
        {
            w.IndentWidth = 0;
            w.BeginArray();
            w.WriteStr("something");
            w.EndArray();
            w.BeginArray();
            w.EndArray();

            w.BeginObject();
            w.WriteInt("x", 99);
            w.EndObject();
        }
        w.EndArray();
        w.IndentWidth = indent;
        
        w.EndObject();
        return w.Buffer;
    };

    std::string resCompact = write(0);
    std::string resFormatted = write(2);

    std::string expCompact = "{dec: 12345, bin: 0b00010101, hex: 0x01c8, flt: 3.14159, flt2: NaN, flt3: Infinity, str: \"hello world! does escaping work? quotes \\\" and control \\r\\\n\\tEmoji: 🙌.\", nested_obj: {thing: {ver: 987, rev: 789}, empty_obj: {}, coords: [123, 456, 789], end: 1}, nest_array: [[\"something\"], [], {x: 99}]}";
    std::string expFormatted = R"({
  dec: 12345,
  bin: 0b00010101,
  hex: 0x01c8,
  flt: 3.14159,
  flt2: NaN,
  flt3: Infinity,
  str: "hello world! does escaping work? quotes \" and control \r\
\tEmoji: 🙌.",
  nested_obj: {
    thing: {ver: 987, rev: 789},
    empty_obj: {
    },
    coords: [123, 456, 789],
    end: 1
  },
  nest_array: [["something"], [], {x: 99}]
})";
    
    // for (int i = 0; i < expFormatted.size(); i++) {
    //     if(resultFormatted[i]!=expFormatted[i]){
    //         printf("ERR AT i %d: %c != %c\n",i,resultFormatted[i], expFormatted[i]);
    //     }
    // }
    CHECK(resCompact == expCompact);
    CHECK(resFormatted == expFormatted);
}

TEST_CASE("serializer for std::unordered_map with int/string key") {
    std::string inputStr = R"({
        str_map: { hello: 1, world: 2, foo: 3 },
        int_map: { '10': 'ten', '20': 'twenty', '30': 'thirty' }
    })";
    
    auto rd = yson::Reader(inputStr);
    rd.ReadExpect(yson::kTypeObject);
    
    rd.ReadNext();
    auto str_map = rd.Parse<std::unordered_map<std::string, int>>();
    CHECK(str_map["hello"] == 1);
    CHECK(str_map["world"] == 2);
    CHECK(str_map["foo"] == 3);

    rd.ReadNext();
    auto int_map = rd.Parse<std::unordered_map<int, std::string>>();
    CHECK(int_map[10] == "ten");
    CHECK(int_map[20] == "twenty");
    CHECK(int_map[30] == "thirty");
    
    yson::Writer wr;
    wr.BeginObject();
    wr.Write("str_map", str_map);
    wr.Write("int_map", int_map);
    wr.EndObject();

    auto rrd = yson::Reader(wr.Buffer);
    rrd.ReadNext();
    rrd.Skip();
    CHECK(rrd.Pos == rrd.Len);
}

TEST_CASE("serializer for glm vector") {
    auto rd = yson::Reader("{ v3i: [1, 2, 3], v3f: [1.2, 3, -4], bad_elem: [3, 4, 'abc'], bad_dim: [1, 2, 3, 4, 5] }");
    rd.ReadExpect(yson::kTypeObject);

    rd.ReadNext();
    CHECK_UNARY(rd.Parse<glm::ivec3>() == glm::ivec3(1, 2, 3));

    rd.ReadNext();
    CHECK_UNARY(rd.Parse<glm::vec3>() == glm::vec3(1.2, 3, -4));

    rd.ReadNext();
    CHECK_THROWS(rd.Parse<glm::vec3>());

    rd.ReadNext();
    CHECK_THROWS(rd.Parse<glm::vec3>());
}