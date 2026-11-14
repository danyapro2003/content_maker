#include "storyrunstore.h"
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <stdexcept>

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void testStoryRunStore()
{
    QTemporaryDir temporary;
    StoryRunStore store;
    require(store.open(temporary.path(), {{"source", "a"}, {"transcript", "one"}}), "store cannot open");
    const auto key = StoryRunStore::hash({{"prompt", "index"}});
    require(store.save("index", key, {{"events", 3}}), "stage not saved");
    require(store.load("index", key)["data"].toObject()["events"] == 3, "stage not reusable");
    require(store.load("index", StoryRunStore::hash({{"prompt", "changed"}})).isEmpty(), "prompt change reused stage");
    require(!store.save("../escape", key, {}), "cache allowed path traversal");
    require(store.save("request", key, {{"request_id", "one"}}, "pending"), "pending not saved");
    require(store.load("request", key).isEmpty() && store.load("request", key, false)["status"] == "pending", "pending treated as complete");
    const auto previous = store.directory();
    require(store.open(temporary.path(), {{"source", "a"}, {"transcript", "two"}})
        && store.load("index", key).isEmpty(), "changed transcript reused map");
    require(store.open(temporary.path(), {{"source", "a"}, {"transcript", "one"}}), "store cannot reopen");
    QFile file(previous + "/index_" + key + ".json");
    require(file.open(QIODevice::WriteOnly), "cannot corrupt fixture"); file.write("{broken"); file.close();
    require(store.load("index", key).isEmpty(), "corrupted stage reused");
}
