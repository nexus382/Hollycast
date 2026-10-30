/*
 * Portions Copyright 2026 The Hollycast Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "oslib/storage.h"
#include "shader_chain.h"
#include "stbi.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>

namespace postprocess
{
namespace
{
struct Format
{
	const char *alias;
	const char *name;
	uint32_t bytes;
};
constexpr Format formats[]{{"R8", "R8_UNORM", 1},
						   {"R16", "R16_UNORM", 2},
						   {"R16F", "R16_SFLOAT", 2},
						   {"R32F", "R32_SFLOAT", 4},
						   {"R32I", "R32_SINT", 4},
						   {"R32U", "R32_UINT", 4},
						   {"RG8", "R8G8_UNORM", 2},
						   {"RG16", "R16G16_UNORM", 4},
						   {"RG16F", "R16G16_SFLOAT", 4},
						   {"RG32F", "R32G32_SFLOAT", 8},
						   {"RGBA8", "R8G8B8A8_UNORM", 4},
						   {"RGBA8_SRGB", "R8G8B8A8_SRGB", 4},
						   {"RGBA16", "R16G16B16A16_UNORM", 8},
						   {"RGBA16F", "R16G16B16A16_SFLOAT", 8},
						   {"RGBA32F", "R32G32B32A32_SFLOAT", 16},
						   {"RGBA32I", "R32G32B32A32_SINT", 16},
						   {"RGBA32U", "R32G32B32A32_UINT", 16},
						   {"RGB10A2", "A2B10G10R10_UNORM_PACK32", 4},
						   {"RG11B10F", "B10G11R11_UFLOAT_PACK32", 4}};
bool fail(std::string &error, const std::string &value)
{
	error = value;
	return false;
}
struct HostInput
{
	std::array<bool, 256> keys{}, toggles{};
	std::array<uint64_t, 256> presses{};
	std::array<uint64_t, 5> mousePresses{};
	std::array<bool, 5> mouseToggles{};
	float x = 0, y = 0, wheel = 0;
	uint32_t buttons = 0;
};
std::mutex inputMutex;
HostInput hostInput;
uint8_t virtualKey(uint8_t hid)
{
	if (hid >= 4 && hid <= 29)
		return hid - 4 + 'A';
	if (hid >= 30 && hid <= 38)
		return hid - 30 + '1';
	if (hid >= 58 && hid <= 69)
		return hid - 58 + 112;
	switch (hid)
	{
	case 39:
		return '0';
	case 40:
		return 13;
	case 41:
		return 27;
	case 42:
		return 8;
	case 43:
		return 9;
	case 44:
		return 32;
	case 73:
		return 45;
	case 74:
		return 36;
	case 75:
		return 33;
	case 76:
		return 46;
	case 77:
		return 35;
	case 78:
		return 34;
	case 79:
		return 39;
	case 80:
		return 37;
	case 81:
		return 40;
	case 82:
		return 38;
	case 224:
		return 162;
	case 225:
		return 160;
	case 226:
		return 164;
	case 227:
		return 91;
	case 228:
		return 163;
	case 229:
		return 161;
	case 230:
		return 165;
	case 231:
		return 92;
	case 45:
		return 189;
	case 46:
		return 187;
	case 47:
		return 219;
	case 48:
		return 221;
	case 49:
		return 220;
	case 51:
		return 186;
	case 52:
		return 222;
	case 53:
		return 192;
	case 54:
		return 188;
	case 55:
		return 190;
	case 56:
		return 191;
	default:
		return 0;
	}
}
// Dimensions are package data, not executable host code. A bounded arithmetic
// parser retains runtime viewport expressions without evaluating arbitrary code.
class SizeExpression
{
  public:
	SizeExpression(const std::string &text, const ShaderFrameInfo &frame, int maximum)
		: text(text), frame(frame), maximum(maximum)
	{
	}
	bool parse(int &value)
	{
		const double result = expression(0);
		whitespace();
		if (!valid || position != text.size() || !std::isfinite(result) || result < 1 || result > maximum)
			return false;
		value = static_cast<int>(result);
		return value > 0;
	}

  private:
	const std::string &text;
	const ShaderFrameInfo &frame;
	int maximum;
	size_t position = 0;
	bool valid = true;
	void whitespace()
	{
		while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])))
			++position;
	}
	bool take(char c)
	{
		whitespace();
		if (position < text.size() && text[position] == c)
		{
			++position;
			return true;
		}
		return false;
	}
	double expression(unsigned depth)
	{
		if (depth > 32)
		{
			valid = false;
			return 0;
		}
		double value = product(depth + 1);
		for (;;)
		{
			if (take('+'))
				value += product(depth + 1);
			else if (take('-'))
				value -= product(depth + 1);
			else
				break;
		}
		whitespace();
		if (position + 1 < text.size() && (text.substr(position, 2) == ">>" || text.substr(position, 2) == "<<"))
		{
			const bool right = text[position] == '>';
			position += 2;
			const double shift = expression(depth + 1);
			if (shift < 0 || shift > 31 || value < 0 || value > 2147483647.0 || std::floor(shift) != shift)
				valid = false;
			else
				value = right ? static_cast<uint32_t>(value) >> static_cast<unsigned>(shift)
							  : static_cast<double>(static_cast<uint64_t>(value) << static_cast<unsigned>(shift));
		}
		return value;
	}
	double product(unsigned depth)
	{
		double value = atom(depth + 1);
		for (;;)
		{
			if (take('*'))
				value *= atom(depth + 1);
			else if (take('/'))
			{
				const double divisor = atom(depth + 1);
				if (divisor == 0)
					valid = false;
				else
					value /= divisor;
			}
			else if (take('%'))
			{
				const double divisor = atom(depth + 1);
				if (divisor == 0)
					valid = false;
				else
					value = std::fmod(value, divisor);
			}
			else
				break;
		}
		return value;
	}
	double atom(unsigned depth)
	{
		if (depth > 32)
		{
			valid = false;
			return 0;
		}
		if (take('-'))
			return -atom(depth + 1);
		if (take('+'))
			return atom(depth + 1);
		if (take('('))
		{
			const double value = expression(depth + 1);
			if (!take(')'))
				valid = false;
			return value;
		}
		whitespace();
		const size_t begin = position;
		while (position < text.size() && (std::isalnum(static_cast<unsigned char>(text[position])) ||
										  text[position] == '_' || text[position] == '.'))
			++position;
		const std::string token = text.substr(begin, position - begin);
		if (token.empty())
		{
			valid = false;
			return 0;
		}
		if (take('('))
		{
			const double a = expression(depth + 1);
			double b = 0;
			const bool pair = take(',');
			if (pair)
				b = expression(depth + 1);
			if (!take(')'))
				valid = false;
			if (token == "min" && pair)
				return std::min(a, b);
			if (token == "max" && pair)
				return std::max(a, b);
			if (token == "ceil" && !pair)
				return std::ceil(a);
			if (token == "floor" && !pair)
				return std::floor(a);
			if ((token == "uint" || token == "int") && !pair)
				return std::trunc(a);
			if (token == "float" && !pair)
				return a;
			valid = false;
			return 0;
		}
		if (token == "OutputSize.x" || token == "FinalViewportSize.x" || token == "BUFFER_WIDTH")
			return frame.viewportWidth;
		if (token == "OutputSize.y" || token == "FinalViewportSize.y" || token == "BUFFER_HEIGHT")
			return frame.viewportHeight;
		if (token == "SourceSize.x" || token == "OriginalSize.x")
			return frame.sourceWidth;
		if (token == "SourceSize.y" || token == "OriginalSize.y")
			return frame.sourceHeight;
		float value = 0;
		if (!parseShaderFloat(token, value))
			valid = false;
		return value;
	}
};

} // namespace
bool resolveShaderSizeExpression(const std::string &expression, const ShaderFrameInfo &frame, int &size,
								 std::string &error, int maximum)
{
	if (SizeExpression(expression, frame, maximum).parse(size))
		return true;
	return fail(error, "Invalid shader target size expression: " + expression);
}

bool resolveShaderDispatchGroups(const ShaderPass &pass, const ShaderFrameInfo &frame, std::array<uint32_t, 3> &groups,
								 std::string &error)
{
	int width, height;
	if (!resolveTargetSize(pass, frame, width, height, error))
		return false;
	const uint32_t invocations[]{static_cast<uint32_t>(width), static_cast<uint32_t>(height), pass.dispatchZ};
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		if (pass.localSize[axis] == 0)
			return fail(error, "Compute workgroup dimensions must be positive.");
		if (pass.dispatchExpressions[axis].empty())
			groups[axis] = (invocations[axis] + pass.localSize[axis] - 1) / pass.localSize[axis];
		else
		{
			int count;
			if (!resolveShaderSizeExpression(pass.dispatchExpressions[axis], frame, count, error, 65535))
				return false;
			groups[axis] = static_cast<uint32_t>(count);
		}
	}
	return true;
}

std::string canonicalShaderFormat(const std::string &value)
{
	std::string name = value;
	std::transform(name.begin(), name.end(), name.begin(),
				   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	if (name == "R11G11B10F")
		name = "RG11B10F";
	if (name == "SRGBA8")
		name = "RGBA8_SRGB";
	if (name == "RGBA8_UNORM")
		name = "RGBA8";
	if (name == "RGBA16_UNORM")
		name = "RGBA16";
	if (name == "RGBA16_FLOAT")
		name = "RGBA16F";
	if (name == "RGBA32_FLOAT")
		name = "RGBA32F";
	for (const auto &format : formats)
		if (name == format.alias || name == format.name)
			return format.name;
	return {};
}
uint32_t shaderFormatBytes(const std::string &name)
{
	const auto canonical = canonicalShaderFormat(name);
	for (const auto &format : formats)
		if (canonical == format.name)
			return format.bytes;
	return 0;
}
ShaderNumericType shaderFormatNumericType(const std::string &name)
{
	const auto format = canonicalShaderFormat(name);
	return format.find("_UINT") != std::string::npos   ? ShaderNumericType::UnsignedInteger
		   : format.find("_SINT") != std::string::npos ? ShaderNumericType::SignedInteger
													   : ShaderNumericType::Float;
}

bool loadShaderTexture(const ShaderPresetTexture &config, ShaderTextureData &result, std::string &error)
{
	error.clear();
	std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(config.path, "rb"));
	if (!file)
		return fail(error, "Required shader texture is missing: " + config.path);
	const auto length = file->size();
	if (length <= 0 || static_cast<uint64_t>(length) > MaxShaderLookupFileBytes)
		return fail(error, "Shader texture exceeds the 64 MiB file limit: " + config.path);
	ShaderTextureData data;
	data.type = config.type;
	const bool cube = config.type == SlangResourceType::SamplerCube || config.type == SlangResourceType::TextureCube;
	const bool volume = config.type == SlangResourceType::Sampler3D || config.type == SlangResourceType::Texture3D;
	const bool array =
		config.type == SlangResourceType::Sampler2DArray || config.type == SlangResourceType::Texture2DArray;
	const auto extension = config.path.find_last_of('.');
	if (extension != std::string::npos && config.path.substr(extension) == ".cube")
	{
		std::string text(static_cast<size_t>(length), '\0');
		if (file->read(text.data(), 1, text.size()) != text.size())
			return fail(error, "Unable to read LUT: " + config.path);
		std::istringstream lines(text);
		lines.imbue(std::locale::classic());
		std::vector<float> pixels;
		uint32_t side = 0;
		for (std::string line; std::getline(lines, line);)
		{
			std::istringstream values(line);
			values.imbue(std::locale::classic());
			std::string token;
			if (!(values >> token) || token[0] == '#' || token == "TITLE")
				continue;
			if (token == "LUT_3D_SIZE")
			{
				if (!(values >> side) || side < 2 || side > 256)
					return fail(error, "Invalid 3D LUT size: " + config.path);
				continue;
			}
			if (token == "DOMAIN_MIN" || token == "DOMAIN_MAX")
			{
				float x, y, z;
				if (!(values >> x >> y >> z) ||
					(token == "DOMAIN_MIN" ? (x != 0 || y != 0 || z != 0) : (x != 1 || y != 1 || z != 1)))
					return fail(error, "Shader LUT requires a normalized 0..1 domain: " + config.path);
				continue;
			}
			float r, g, b;
			if (!parseShaderFloat(token, r) || !(values >> g >> b) || !std::isfinite(g) || !std::isfinite(b) ||
				side == 0)
				return fail(error, "Invalid 3D LUT data: " + config.path);
			if (pixels.size() >= static_cast<size_t>(side) * side * side * 4)
				return fail(error, "Too many LUT entries: " + config.path);
			pixels.insert(pixels.end(), {r, g, b, 1.f});
		}
		if (side == 0 || pixels.size() != static_cast<size_t>(side) * side * side * 4 || !volume ||
			config.numericType != ShaderNumericType::Float)
			return fail(error, "3D LUT dimensions or entry count do not match the declared texture: " + config.path);
		data.width = data.height = data.depth = side;
		data.formatName = "R32G32B32A32_SFLOAT";
		data.pixels.resize(pixels.size() * sizeof(float));
		std::memcpy(data.pixels.data(), pixels.data(), data.pixels.size());
	}
	else
	{
		int width = 0, height = 0, channels = 0;
		// Inspect dimensions before decoding; compressed packages must not force an unbounded allocation.
		if (!stbi_info_from_callbacks(&STBI::callbacks, file.get(), &width, &height, &channels) ||
			!validateShaderLookupTexture(width, height, error))
			return fail(error, "Invalid shader texture dimensions: " + config.path + ": " + error);
		file->seek(0, SEEK_SET);
		std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
			stbi_load_from_file(file.get(), &width, &height, &channels, STBI_rgb_alpha), stbi_image_free);
		if (!pixels)
			return fail(error, "Unable to decode required shader texture: " + config.path);
		data.width = config.width ? config.width : static_cast<uint32_t>(width);
		data.height = config.height ? config.height : static_cast<uint32_t>(height);
		data.depth = volume ? config.depth : 1;
		data.layers = cube ? 6 : array ? config.depth : 1;
		if (cube && !config.width)
			data.width = data.height = static_cast<uint32_t>(std::min(width, height));
		if (config.type == SlangResourceType::Sampler1D || config.type == SlangResourceType::Texture1D)
			data.height = 1;
		const uint64_t slices = static_cast<uint64_t>(data.depth) * data.layers;
		const uint64_t bytes = static_cast<uint64_t>(data.width) * data.height * slices * 4;
		if (data.width == 0 || data.height == 0 || data.depth == 0 || bytes > MaxShaderChainTargetBytes / 2 ||
			width % data.width || height % data.height ||
			static_cast<uint64_t>(width / data.width) * (height / data.height) != slices)
			return fail(error, "Shader texture image does not match its declared slices/faces: " + config.path);
		data.pixels.resize(static_cast<size_t>(bytes));
		const uint32_t columns = width / data.width;
		for (uint32_t slice = 0; slice < slices; ++slice)
			for (uint32_t y = 0; y < data.height; ++y)
			{
				const auto source = ((slice / columns * data.height + y) * width + slice % columns * data.width) * 4;
				const auto target = (static_cast<size_t>(slice) * data.height + y) * data.width * 4;
				std::memcpy(data.pixels.data() + target, pixels.get() + source, data.width * 4);
			}
		if (config.numericType != ShaderNumericType::Float)
		{
			if (data.pixels.size() > MaxShaderChainTargetBytes / 8)
				return fail(error, "Decoded integer shader image exceeds the 256 MiB limit.");
			std::vector<uint8_t> integers(data.pixels.size() * 4);
			for (size_t i = 0; i < data.pixels.size(); ++i)
			{
				uint32_t value = data.pixels[i];
				std::memcpy(integers.data() + i * 4, &value, 4);
			}
			data.pixels = std::move(integers);
			data.formatName =
				config.numericType == ShaderNumericType::SignedInteger ? "R32G32B32A32_SINT" : "R32G32B32A32_UINT";
		}
	}
	result = std::move(data);
	return true;
}
std::shared_ptr<const ShaderTextureData> loadShaderTextureData(const ShaderPresetTexture& texture, std::string& error)
{
	if (texture.decoded) return texture.decoded;
	auto data = std::make_shared<ShaderTextureData>();
	if (!loadShaderTexture(texture, *data, error)) return {};
	return data;
}

bool preflightShaderAssets(ShaderChain &chain, std::string &error)
{
	uint64_t bytes = 0;
	for (auto &texture : chain.textures)
	{
		auto data = loadShaderTextureData(texture, error);
		if (!data) return false;
		if (data->pixels.size() > MaxShaderChainTargetBytes - bytes)
			return fail(error, "Shader assets exceed the 512 MiB chain budget.");
		bytes += data->pixels.size();
		texture.decoded = std::move(data);
	}
	return true;
}

void setShaderHostKey(uint8_t hidKey, bool pressed)
{
	const uint8_t key = virtualKey(hidKey);
	if (!key)
		return;
	std::lock_guard<std::mutex> lock(inputMutex);
	if (pressed && !hostInput.keys[key])
	{
		hostInput.toggles[key] = !hostInput.toggles[key];
		++hostInput.presses[key];
	}
	const bool wasShift = hostInput.keys[16], wasControl = hostInput.keys[17], wasAlt = hostInput.keys[18];
	hostInput.keys[key] = pressed;
	// Virtual-key modifiers have generic and side-specific spellings.
	hostInput.keys[16] = hostInput.keys[160] || hostInput.keys[161];
	hostInput.keys[17] = hostInput.keys[162] || hostInput.keys[163];
	hostInput.keys[18] = hostInput.keys[164] || hostInput.keys[165];
	const bool previous[]{wasShift, wasControl, wasAlt};
	for (unsigned i = 0; i < 3; ++i)
		if (!previous[i] && hostInput.keys[16 + i])
		{
			hostInput.toggles[16 + i] = !hostInput.toggles[16 + i];
			++hostInput.presses[16 + i];
		}
}
void setShaderHostMouse(float x, float y, uint32_t buttons, float wheel)
{
	std::lock_guard<std::mutex> lock(inputMutex);
	hostInput.x = x;
	hostInput.y = y;
	for (unsigned i = 0; i < 5; ++i)
		if ((buttons & (1u << i)) && !(hostInput.buttons & (1u << i)))
		{
			hostInput.mouseToggles[i] = !hostInput.mouseToggles[i];
			++hostInput.mousePresses[i];
		}
	hostInput.buttons = buttons;
	hostInput.wheel += wheel;
}
void updateShaderHostParameters(std::vector<ShaderParameter> &parameters, const ShaderFrameInfo &frame)
{
	using Clock = std::chrono::steady_clock;
	static const auto start = Clock::now();
	// Each executor is confined to its render thread; timestamps never cross clocks.
	thread_local auto previous = Clock::now();
	thread_local float previousX = 0, previousY = 0, previousWheel = 0;
	thread_local std::array<uint64_t, 256> previousPresses{};
	thread_local std::array<uint64_t, 5> previousMousePresses{};
	const auto now = Clock::now();
	const float time = std::chrono::duration<float, std::milli>(now - start).count();
	const float delta = std::chrono::duration<float, std::milli>(now - previous).count();
	previous = now;
	HostInput input;
	{
		std::lock_guard<std::mutex> lock(inputMutex);
		input = hostInput;
	}
	for (auto &parameter : parameters)
	{
		if (parameter.hostSource.empty())
			continue;
		auto &v = parameter.values;
		const float priorValue = v[0];
		v.fill(0.f);
		const auto &source = parameter.hostSource;
		if (source == "timer")
			v[0] = time;
		else if (source == "frametime")
			v[0] = delta;
		else if (source == "framecount")
			v[0] = static_cast<float>(frame.frameCount);
		else if (source == "mousepoint")
		{
			v[0] = input.x;
			v[1] = input.y;
		}
		else if (source == "mousedelta")
		{
			v[0] = input.x - previousX;
			v[1] = input.y - previousY;
		}
		else if (source == "mousewheel")
		{
			v[1] = input.wheel - previousWheel;
			v[0] = priorValue + v[1];
			if (parameter.minimum < parameter.maximum)
				v[0] = std::clamp(v[0], parameter.minimum, parameter.maximum);
		}
		else if (source == "mousebutton")
		{
			const auto key = std::min(parameter.hostKey, 4u);
			v[0] = (parameter.hostToggle  ? input.mouseToggles[key]
					: parameter.hostPress ? input.mousePresses[key] != previousMousePresses[key]
										  : (input.buttons & (1u << key)) != 0)
					   ? 1.f
					   : 0.f;
		}
		else if (source == "key")
		{
			const auto key = std::min(parameter.hostKey, 255u);
			v[0] = (parameter.hostToggle  ? input.toggles[key]
					: parameter.hostPress ? input.presses[key] != previousPresses[key]
										  : input.keys[key])
					   ? 1.f
					   : 0.f;
		}
		else if (source == "date")
		{
			const std::time_t stamp = std::time(nullptr);
			std::tm date{};
#ifdef _WIN32
			localtime_s(&date, &stamp);
#else
			localtime_r(&stamp, &date);
#endif
			v[0] = static_cast<float>(date.tm_year + 1900);
			v[1] = static_cast<float>(date.tm_mon + 1);
			v[2] = static_cast<float>(date.tm_mday);
			v[3] = static_cast<float>(date.tm_hour * 3600 + date.tm_min * 60 + date.tm_sec);
		}
		else if (source == "random")
		{
			uint32_t seed = 2166136261u;
			for (unsigned char c : parameter.name)
				seed = (seed ^ c) * 16777619u;
			uint32_t bits = frame.frameCount * 747796405u + 2891336453u + seed;
			bits = ((bits >> ((bits >> 28u) + 4u)) ^ bits) * 277803737u;
			const float value = static_cast<float>((bits >> 22u) ^ bits) / 4294967295.f;
			v[0] = parameter.minimum + value * (parameter.maximum - parameter.minimum);
			if (parameter.type == SlangParameterType::Int)
				v[0] = std::min(parameter.maximum,
								std::floor(parameter.minimum + value * (parameter.maximum - parameter.minimum + 1.f)));
		}
		parameter.value = v[0];
	}
	previousX = input.x;
	previousY = input.y;
	previousWheel = input.wheel;
	previousPresses = input.presses;
	previousMousePresses = input.mousePresses;
}
} // namespace postprocess
