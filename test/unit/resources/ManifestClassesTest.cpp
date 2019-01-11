/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexResources.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

TEST(ManifestClassesTest, exported) {
  const auto& manifest_filename = std::getenv("test_manifest_path");
  const auto& class_info = get_manifest_class_info(manifest_filename);

  const auto& tag_infos = class_info.component_tags;
  EXPECT_EQ(tag_infos.size(), 5);

  EXPECT_EQ(tag_infos[0].tag, ComponentTag::Activity);
  EXPECT_EQ(tag_infos[0].classname, "Ltest1;");
  EXPECT_TRUE(tag_infos[0].is_exported);
  EXPECT_FALSE(tag_infos[0].has_intent_filters);

  EXPECT_EQ(tag_infos[1].tag, ComponentTag::Activity);
  EXPECT_EQ(tag_infos[1].classname, "Ltest2;");
  EXPECT_FALSE(tag_infos[1].is_exported);
  EXPECT_FALSE(tag_infos[1].has_intent_filters);

  EXPECT_EQ(tag_infos[2].tag, ComponentTag::Activity);
  EXPECT_EQ(tag_infos[2].classname, "Ltest3;");
  EXPECT_FALSE(tag_infos[2].is_exported);
  EXPECT_TRUE(tag_infos[2].has_intent_filters);

  EXPECT_EQ(tag_infos[3].tag, ComponentTag::Activity);
  EXPECT_EQ(tag_infos[3].classname, "Ltest4;");
  EXPECT_TRUE(tag_infos[3].is_exported);
  EXPECT_FALSE(tag_infos[3].has_intent_filters);

  EXPECT_EQ(tag_infos[4].tag, ComponentTag::Provider);
  EXPECT_EQ(tag_infos[4].classname, "Lcom/example/x/Foo;");
  EXPECT_FALSE(tag_infos[4].is_exported);
  EXPECT_THAT(tag_infos[4].authority_classes,
              ::testing::UnorderedElementsAre("Lcom/example/x/Foo;",
                                              "Lcom/example/y/Bar;"));
}
