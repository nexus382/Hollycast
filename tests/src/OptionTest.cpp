// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "cfg/ini.h"
#include "cfg/option.h"

namespace {

enum class TestEnum { First = 1, Second = 2 };

config::Option<bool, false> boolOption("bool", true, "option-test");
config::Option<int, false> intOption("int", 17, "option-test");
config::Option<int64_t, false> int64Option("int64", 1234567890123ll, "option-test");
config::Option<TestEnum, false> enumOption("enum", TestEnum::First, "option-test");
config::Option<std::string, false> stringOption("string", "default", "option-test");
config::Option<float, false> floatOption("float", 1.25f, "option-test");
config::Option<std::vector<std::string>, false> listOption("list", {"default"}, "option-test");

class OptionTest : public testing::Test {
protected:
	~OptionTest() override
	{
		resetOptions();
		config::deleteSection("option-test");
	}

	void SetUp() override
	{
		config::deleteSection("option-test");
		resetOptions();
	}

	static void resetOptions()
	{
		boolOption.reset();
		intOption.reset();
		int64Option.reset();
		enumOption.reset();
		stringOption.reset();
		floatOption.reset();
		listOption.reset();
	}
};

TEST_F(OptionTest, DefaultsAndSetValuesCoverEverySupportedKind)
{
	EXPECT_TRUE(boolOption.get());
	EXPECT_EQ(17, intOption.get());
	EXPECT_EQ(1234567890123ll, int64Option.get());
	EXPECT_EQ(TestEnum::First, enumOption.get());
	EXPECT_EQ("default", stringOption.get());
	EXPECT_FLOAT_EQ(1.25f, floatOption.get());
	EXPECT_EQ((std::vector<std::string>{"default"}), listOption.get());

	boolOption.set(false);
	intOption.set(-42);
	int64Option.set(9876543210123ll);
	enumOption.set(TestEnum::Second);
	stringOption.set("changed value");
	floatOption.set(3.5f);
	listOption.set({"one", "two", "three"});

	EXPECT_FALSE(boolOption.get());
	EXPECT_EQ(-42, intOption.get());
	EXPECT_EQ(9876543210123ll, int64Option.get());
	EXPECT_EQ(TestEnum::Second, enumOption.get());
	EXPECT_EQ("changed value", stringOption.get());
	EXPECT_FLOAT_EQ(3.5f, floatOption.get());
	EXPECT_EQ((std::vector<std::string>{"one", "two", "three"}), listOption.get());
}

TEST_F(OptionTest, InvalidIntKeepsCurrentValueAndInvalidFloatLoadsZero)
{
	intOption.set(29);
	floatOption.set(2.75f);
	config::setTransient("option-test", "int", "not-a-number");
	config::setTransient("option-test", "float", "not-a-number");

	intOption.load();
	floatOption.load();

	EXPECT_EQ(29, intOption.get());
	EXPECT_FLOAT_EQ(0.f, floatOption.get());
}

TEST_F(OptionTest, IniRoundTripLoadsValuesAndRetainsUnknownKeys)
{
	boolOption.set(false);
	intOption.set(-42);
	int64Option.set(9876543210123ll);
	enumOption.set(TestEnum::Second);
	stringOption.set("value with spaces = preserved");
	floatOption.set(3.5f);
	listOption.set({"one", "two", "three"});

	config::IniFile source;
	source.set("option-test", "bool", boolOption.get());
	source.set("option-test", "int", intOption.get());
	source.set("option-test", "int64", int64Option.get());
	source.set("option-test", "enum", static_cast<int>(enumOption.get()));
	source.set("option-test", "string", stringOption.get());
	source.set("option-test", "float", floatOption.get());
	std::string serializedList;
	for (const auto& value : listOption.get()) {
		if (!serializedList.empty()) serializedList += ';';
		serializedList += value;
	}
	source.set("option-test", "list", serializedList);
	source.set("option-test", "unknown", "kept");

	std::string serialized;
	source.save(serialized);
	config::IniFile loaded;
	loaded.load(serialized);
	ASSERT_TRUE(loaded.hasEntry("option-test", "unknown"));
	EXPECT_EQ("kept", loaded.getString("option-test", "unknown"));
	EXPECT_EQ("value with spaces = preserved", loaded.getString("option-test", "string"));

	resetOptions();
	config::setTransient("option-test", "bool", loaded.getString("option-test", "bool"));
	config::setTransient("option-test", "int", loaded.getString("option-test", "int"));
	config::setTransient("option-test", "int64", loaded.getString("option-test", "int64"));
	config::setTransient("option-test", "enum", loaded.getString("option-test", "enum"));
	config::setTransient("option-test", "string", loaded.getString("option-test", "string"));
	config::setTransient("option-test", "float", loaded.getString("option-test", "float"));
	config::setTransient("option-test", "list", loaded.getString("option-test", "list"));
	boolOption.load();
	intOption.load();
	int64Option.load();
	enumOption.load();
	stringOption.load();
	floatOption.load();
	listOption.load();

	EXPECT_FALSE(boolOption.get());
	EXPECT_EQ(-42, intOption.get());
	EXPECT_EQ(9876543210123ll, int64Option.get());
	EXPECT_EQ(TestEnum::Second, enumOption.get());
	EXPECT_EQ("value with spaces = preserved", stringOption.get());
	EXPECT_FLOAT_EQ(3.5f, floatOption.get());
	EXPECT_EQ((std::vector<std::string>{"one", "two", "three"}), listOption.get());
}

TEST_F(OptionTest, MissingIniSectionLeavesOptionsAtTheirDefaults)
{
	boolOption.set(false);
	intOption.set(99);
	stringOption.set("changed");
	resetOptions();

	boolOption.load();
	intOption.load();
	stringOption.load();

	EXPECT_TRUE(boolOption.get());
	EXPECT_EQ(17, intOption.get());
	EXPECT_EQ("default", stringOption.get());
}

} // namespace
