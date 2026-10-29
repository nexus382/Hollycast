// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "types.h"
#include <string>
namespace config
{
extern bool MovableVMUs;
extern bool VmuLayoutEnabled;
struct TestIntOption
{
	int value = 100;
	int &get()
	{
		return value;
	}
	void operator=(int next)
	{
		value = next;
	}
};
extern TestIntOption VmuWindowSize;
extern TestIntOption VmuWindowTransparency;
extern bool FloatVMUs;
extern RenderType RendererType;
int loadInt(const std::string &, const std::string &, int);
void saveInt(const std::string &, const std::string &, int);
void setAutoSave(bool);
} // namespace config
