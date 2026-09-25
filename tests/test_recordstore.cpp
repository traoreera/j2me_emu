// test_recordstore.cpp
// RecordStore (vm/natives.cpp) : persistance disque + énumération. On appelle
// les natives par leur clé du registre (findNative), exactement comme le fait
// l'interpréteur -- sans SDL ni midp_*.cpp : seule la classe RecordStore est
// enregistrée à la main sur le Runtime de test.

#include "framework.h"
#include "../vm/native.h"
#include "../vm/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

using namespace jvm;

namespace
{
    struct Fixture
    {
        Runtime rt{64 * 1024};
        std::string dir;
        Fixture()
        {
            initNatives();
            rt.registerNativeClass("java/lang/Object", "", {}, {});
            rt.registerNativeClass("javax/microedition/rms/RecordStore", "java/lang/Object", {},
                                   {{"__name", "Ljava/lang/String;"}});
            rt.registerNativeClass("javax/microedition/rms/RecordEnumerationImpl", "java/lang/Object", {},
                                   {{"store", "Ljava/lang/String;"}, {"ids", "[I"}, {"pos", "I"}});
            char tmpl[] = "/tmp/j2me_test_rms_XXXXXX";
            dir = mkdtemp(tmpl);
            setRmsDir(dir);
        }
        ~Fixture()
        {
            setRmsDir("");
            std::string cmd = "rm -rf '" + dir + "'";
            (void)system(cmd.c_str());
        }
        Value call(const char *key, Obj *self, std::vector<Value> args)
        {
            NativeFn fn = findNative(key);
            ASSERT_TRUE(static_cast<bool>(fn));
            Value res;
            NativeContext ctx;
            ctx.rt = &rt;
            ctx.args = args.data();
            ctx.nargs = static_cast<int>(args.size());
            ctx.thisObj = self;
            ctx.result = &res;
            fn(&ctx);
            return res;
        }
        Obj *open(const std::string &name)
        {
            return call("javax/microedition/rms/RecordStore.openRecordStore:(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;",
                        nullptr, {Value::fromRef(rt.heap().newString(name)), Value::fromInt(1)}).o;
        }
        int add(Obj *rs, std::vector<uint8_t> bytes)
        {
            Obj *arr = rt.heap().newArray(ObjKind::ByteArray, static_cast<int>(bytes.size()));
            for (size_t i = 0; i < bytes.size(); i++)
                arr->cells[i].u = bytes[i];
            return call("javax/microedition/rms/RecordStore.addRecord:([BII)I", rs,
                        {Value::fromRef(nullptr), Value::fromRef(arr), Value::fromInt(0),
                         Value::fromInt(static_cast<int>(bytes.size()))}).i;
        }
    };

    std::vector<uint8_t> readFile(const std::string &path)
    {
        std::vector<uint8_t> out;
        FILE *f = fopen(path.c_str(), "rb");
        if (!f) return out;
        int c;
        while ((c = fgetc(f)) != EOF) out.push_back(static_cast<uint8_t>(c));
        fclose(f);
        return out;
    }
    void writeFile(const std::string &path, const std::vector<uint8_t> &b)
    {
        FILE *f = fopen(path.c_str(), "wb");
        fwrite(b.data(), 1, b.size(), f);
        fclose(f);
    }
} // namespace

TEST(recordstore_add_persists_exact_format_on_disk)
{
    Fixture fx;
    Obj *rs = fx.open("save1");
    ASSERT_EQ(fx.add(rs, {1, 2, 3}), 1);
    ASSERT_EQ(fx.add(rs, {}), 2);

    std::vector<uint8_t> got = readFile(fx.dir + "/save1.rms");
    std::vector<uint8_t> want = {2, 0, 0, 0,  3, 0, 0, 0, 1, 2, 3,  0, 0, 0, 0};
    ASSERT_TRUE(got == want);
}

TEST(recordstore_loads_existing_file_on_first_open)
{
    Fixture fx;
    // count=1, record de 2 octets {9,8}
    writeFile(fx.dir + "/loaded.rms", {1, 0, 0, 0, 2, 0, 0, 0, 9, 8});
    Obj *rs = fx.open("loaded");
    ASSERT_EQ(fx.call("javax/microedition/rms/RecordStore.getNumRecords:()I", rs, {}).i, 1);
    ASSERT_EQ(fx.call("javax/microedition/rms/RecordStore.getRecordSize:(I)I", rs, {Value::fromRef(nullptr), Value::fromInt(1)}).i, 2);
    Obj *bytes = fx.call("javax/microedition/rms/RecordStore.getRecord:(I)[B", rs, {Value::fromRef(nullptr), Value::fromInt(1)}).o;
    ASSERT_TRUE(bytes != nullptr);
    ASSERT_EQ((int)bytes->cells[0].u, 9);
    ASSERT_EQ((int)bytes->cells[1].u, 8);
}

TEST(recordstore_corrupt_file_is_ignored_not_fatal)
{
    Fixture fx;
    writeFile(fx.dir + "/bad.rms", {5, 0, 0, 0, 200, 0, 0, 0, 1}); // annonce 5 records, un seul tronqué
    Obj *rs = fx.open("bad");
    ASSERT_EQ(fx.call("javax/microedition/rms/RecordStore.getNumRecords:()I", rs, {}).i, 0);
}

TEST(recordstore_delete_removes_file)
{
    Fixture fx;
    Obj *rs = fx.open("gone");
    fx.add(rs, {7});
    ASSERT_TRUE(access((fx.dir + "/gone.rms").c_str(), F_OK) == 0);
    fx.call("javax/microedition/rms/RecordStore.deleteRecordStore:(Ljava/lang/String;)V", nullptr,
            {Value::fromRef(fx.rt.heap().newString("gone"))});
    ASSERT_TRUE(access((fx.dir + "/gone.rms").c_str(), F_OK) != 0);
}

TEST(recordstore_enumeration_walks_ids_in_order)
{
    Fixture fx;
    Obj *rs = fx.open("enum1");
    fx.add(rs, {10});
    fx.add(rs, {20});
    fx.add(rs, {30});
    Obj *en = fx.call("javax/microedition/rms/RecordStore.enumerateRecords:(Ljavax/microedition/rms/RecordFilter;Ljavax/microedition/rms/RecordComparator;Z)Ljavax/microedition/rms/RecordEnumeration;",
                      rs, {Value::fromRef(nullptr), Value::fromRef(nullptr), Value::fromRef(nullptr), Value::fromInt(0)}).o;
    ASSERT_TRUE(en != nullptr);
    ASSERT_EQ(fx.call("javax/microedition/rms/RecordEnumerationImpl.numRecords:()I", en, {}).i, 3);
    for (int expect = 1; expect <= 3; expect++)
    {
        ASSERT_EQ(fx.call("javax/microedition/rms/RecordEnumerationImpl.hasNextElement:()Z", en, {}).i, 1);
        ASSERT_EQ(fx.call("javax/microedition/rms/RecordEnumerationImpl.nextRecordId:()I", en, {}).i, expect);
    }
    ASSERT_EQ(fx.call("javax/microedition/rms/RecordEnumerationImpl.hasNextElement:()Z", en, {}).i, 0);
    // reset() ramène au début
    fx.call("javax/microedition/rms/RecordEnumerationImpl.reset:()V", en, {});
    Obj *first = fx.call("javax/microedition/rms/RecordEnumerationImpl.nextRecord:()[B", en, {}).o;
    ASSERT_TRUE(first != nullptr);
    ASSERT_EQ((int)first->cells[0].u, 10);
}
