#include <string>
#include <type_traits>

#include <gtest/gtest.h>

#include "lyrium/never_destroyed.h"

using lyrium::NeverDestroyed;

namespace
{

int destructions = 0;

struct Tracked
{
    explicit Tracked(int value = 0, std::string label = {})
        : value{value}
        , label{std::move(label)}
    {
    }

    ~Tracked()
    {
        ++destructions;
    }

    int value;
    std::string label;
};

}

// The property everything else rests on. A function-local static of this type
// registers no __cxa_atexit entry, so it never enters the exit sequence at all.
static_assert(std::is_trivially_destructible_v<NeverDestroyed<Tracked>>);
static_assert(std::is_trivially_destructible_v<NeverDestroyed<std::string>>);
static_assert(!std::is_copy_constructible_v<NeverDestroyed<Tracked>>);
static_assert(!std::is_copy_assignable_v<NeverDestroyed<Tracked>>);

TEST(NeverDestroyedTest, DoesNotRunTheDestructorWhenTheHolderDies)
{
    destructions = 0;

    {
        auto held = NeverDestroyed<Tracked>{7};
        EXPECT_EQ(held.get().value, 7);
    }

    EXPECT_EQ(destructions, 0) << "the held object must survive the holder going out of scope";
}

TEST(NeverDestroyedTest, ForwardsConstructorArguments)
{
    auto held = NeverDestroyed<Tracked>{42, std::string{"hello"}};

    EXPECT_EQ(held.get().value, 42);
    EXPECT_EQ(held.get().label, "hello");
}

TEST(NeverDestroyedTest, DefaultConstructsWhenGivenNoArguments)
{
    auto held = NeverDestroyed<Tracked>{};
    EXPECT_EQ(held.get().value, 0);
}

TEST(NeverDestroyedTest, AccessorsAllReachTheSameObject)
{
    auto held = NeverDestroyed<Tracked>{1};

    held.get().value = 99;
    EXPECT_EQ((*held).value, 99);
    EXPECT_EQ(held->value, 99);
    EXPECT_EQ(&held.get(), &*held);
}

TEST(NeverDestroyedTest, StorageIsExactlyTheHeldObject)
{
    // In-object storage, so nothing is allocated. That matters because this DLL
    // must never allocate from the game's CRT heap, which operator new would.
    EXPECT_EQ(sizeof(NeverDestroyed<Tracked>), sizeof(Tracked));
    EXPECT_EQ(alignof(NeverDestroyed<Tracked>), alignof(Tracked));
}

TEST(NeverDestroyedTest, HoldsATypeWithANonTrivialDestructor)
{
    // std::string would normally register cleanup; held this way it does not.
    auto held = NeverDestroyed<std::string>{"a string long enough to need a heap allocation"};
    EXPECT_EQ(held.get().size(), 46u);
}
