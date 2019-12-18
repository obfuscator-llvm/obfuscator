//===- llvm/unittest/ADT/SmallSetTest.cpp ------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// SmallSet unit tests.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallSet.h"
#include "gtest/gtest.h"
#include <string>

using namespace llvm;

TEST(SmallSetTest, Insert) {

  SmallSet<int, 4> s1;

  for (int i = 0; i < 4; i++)
    s1.insert(i);

  for (int i = 0; i < 4; i++)
    s1.insert(i);

  EXPECT_EQ(4u, s1.size());

  for (int i = 0; i < 4; i++)
    EXPECT_EQ(1u, s1.count(i));

  EXPECT_EQ(0u, s1.count(4));
}

TEST(SmallSetTest, Grow) {
  SmallSet<int, 4> s1;

  for (int i = 0; i < 8; i++)
    s1.insert(i);

  EXPECT_EQ(8u, s1.size());

  for (int i = 0; i < 8; i++)
    EXPECT_EQ(1u, s1.count(i));

  EXPECT_EQ(0u, s1.count(8));
}

TEST(SmallSetTest, Erase) {
  SmallSet<int, 4> s1;

  for (int i = 0; i < 8; i++)
    s1.insert(i);

  EXPECT_EQ(8u, s1.size());

  // Remove elements one by one and check if all other elements are still there.
  for (int i = 0; i < 8; i++) {
    EXPECT_EQ(1u, s1.count(i));
    EXPECT_TRUE(s1.erase(i));
    EXPECT_EQ(0u, s1.count(i));
    EXPECT_EQ(8u - i - 1, s1.size());
    for (int j = i + 1; j < 8; j++)
      EXPECT_EQ(1u, s1.count(j));
  }

  EXPECT_EQ(0u, s1.count(8));
}

TEST(SmallSetTest, IteratorInt) {
  SmallSet<int, 4> s1;

  // Test the 'small' case.
  for (int i = 0; i < 3; i++)
    s1.insert(i);

  std::vector<int> V(s1.begin(), s1.end());
  // Make sure the elements are in the expected order.
  std::sort(V.begin(), V.end());
  for (int i = 0; i < 3; i++)
    EXPECT_EQ(i, V[i]);

  // Test the 'big' case by adding a few more elements to switch to std::set
  // internally.
  for (int i = 3; i < 6; i++)
    s1.insert(i);

  V.assign(s1.begin(), s1.end());
  // Make sure the elements are in the expected order.
  std::sort(V.begin(), V.end());
  for (int i = 0; i < 6; i++)
    EXPECT_EQ(i, V[i]);
}

TEST(SmallSetTest, IteratorString) {
  // Test SmallSetIterator for SmallSet with a type with non-trivial
  // ctors/dtors.
  SmallSet<std::string, 2> s1;

  s1.insert("str 1");
  s1.insert("str 2");
  s1.insert("str 1");

  std::vector<std::string> V(s1.begin(), s1.end());
  std::sort(V.begin(), V.end());
  EXPECT_EQ(2u, s1.size());
  EXPECT_EQ("str 1", V[0]);
  EXPECT_EQ("str 2", V[1]);

  s1.insert("str 4");
  s1.insert("str 0");
  s1.insert("str 4");

  V.assign(s1.begin(), s1.end());
  // Make sure the elements are in the expected order.
  std::sort(V.begin(), V.end());
  EXPECT_EQ(4u, s1.size());
  EXPECT_EQ("str 0", V[0]);
  EXPECT_EQ("str 1", V[1]);
  EXPECT_EQ("str 2", V[2]);
  EXPECT_EQ("str 4", V[3]);
}

TEST(SmallSetTest, IteratorIncMoveCopy) {
  // Test SmallSetIterator for SmallSet with a type with non-trivial
  // ctors/dtors.
  SmallSet<std::string, 2> s1;

  s1.insert("str 1");
  s1.insert("str 2");

  auto Iter = s1.begin();
  EXPECT_EQ("str 1", *Iter);
  ++Iter;
  EXPECT_EQ("str 2", *Iter);

  s1.insert("str 4");
  s1.insert("str 0");
  auto Iter2 = s1.begin();
  Iter = std::move(Iter2);
  EXPECT_EQ("str 0", *Iter);
}
