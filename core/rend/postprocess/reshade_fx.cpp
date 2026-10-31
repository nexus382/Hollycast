/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "reshade_fx.h"
#include "effect_preprocessor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace postprocess {
namespace {

enum class TokenKind { Identifier, Number, String, Symbol, Directive, End };

struct Token {
	TokenKind kind;
	std::string text;
	size_t begin;
	size_t end;
	size_t line;
};

bool isIdentifierStart(char c)
{
	return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isIdentifierPart(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string lowercase(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

bool isIncludeDirective(const std::string& directive)
{
	size_t index = 0;
	while (index < directive.size() && (directive[index] == '#' || std::isspace(static_cast<unsigned char>(directive[index]))))
		++index;
	return lowercase(directive.substr(index, 7)) == "include";
}

std::string trim(const std::string& value)
{
	const size_t first = value.find_first_not_of(" \t\r");
	if (first == std::string::npos)
		return {};
	const size_t last = value.find_last_not_of(" \t\r");
	return value.substr(first, last - first + 1);
}

class PreprocessorExpressionParser {
public:
	PreprocessorExpressionParser(std::string_view expression,
			const std::unordered_map<std::string, std::string>& defines, unsigned depth = 0)
		: expression(expression), defines(defines), depth(depth) {}

	bool evaluate(bool& result)
	{
		long long value = 0;
		if (!evaluateValue(value))
			return false;
		result = value != 0;
		return true;
	}

	bool evaluateValue(long long& value)
	{
		if (!parseLogicalOr(value))
			return false;
		skipWhitespace();
		if (position != expression.size())
			return false;
		return true;
	}

private:
	void skipWhitespace()
	{
		while (position < expression.size() && std::isspace(static_cast<unsigned char>(expression[position])))
			++position;
	}

	bool match(std::string_view token)
	{
		skipWhitespace();
		if (expression.substr(position, token.size()) != token)
			return false;
		position += token.size();
		return true;
	}

	bool matchSingle(char token, char doubled)
	{
		skipWhitespace();
		if (position >= expression.size() || expression[position] != token
				|| (position + 1 < expression.size() && expression[position + 1] == doubled))
			return false;
		++position;
		return true;
	}

	bool takeIdentifier(std::string& identifier)
	{
		skipWhitespace();
		if (position >= expression.size() || !isIdentifierStart(expression[position]))
			return false;
		const size_t begin = position++;
		while (position < expression.size() && isIdentifierPart(expression[position]))
			++position;
		identifier.assign(expression.substr(begin, position - begin));
		return true;
	}

	bool parseLogicalOr(long long& value)
	{
		if (!parseLogicalAnd(value))
			return false;
		while (match("||")) {
			long long right = 0;
			if (!parseLogicalAnd(right))
				return false;
			value = value || right;
		}
		return true;
	}

	bool parseLogicalAnd(long long& value)
	{
		if (!parseBitwiseOr(value))
			return false;
		while (match("&&")) {
			long long right = 0;
			if (!parseBitwiseOr(right))
				return false;
			value = value && right;
		}
		return true;
	}

	bool parseBitwiseOr(long long& value)
	{
		if (!parseBitwiseXor(value))
			return false;
		while (matchSingle('|', '|')) {
			long long right = 0;
			if (!parseBitwiseXor(right))
				return false;
			value |= right;
		}
		return true;
	}

	bool parseBitwiseXor(long long& value)
	{
		if (!parseBitwiseAnd(value))
			return false;
		while (match("^")) {
			long long right = 0;
			if (!parseBitwiseAnd(right))
				return false;
			value ^= right;
		}
		return true;
	}

	bool parseBitwiseAnd(long long& value)
	{
		if (!parseEquality(value))
			return false;
		while (matchSingle('&', '&')) {
			long long right = 0;
			if (!parseEquality(right))
				return false;
			value &= right;
		}
		return true;
	}

	bool parseEquality(long long& value)
	{
		if (!parseRelational(value))
			return false;
		for (;;) {
			const bool equal = match("==");
			const bool notEqual = !equal && match("!=");
			if (!equal && !notEqual)
				return true;
			long long right = 0;
			if (!parseRelational(right))
				return false;
			value = equal ? value == right : value != right;
		}
	}

	bool parseRelational(long long& value)
	{
		if (!parseShift(value))
			return false;
		for (;;) {
			int operation = 0;
			if (match("<=")) operation = 1;
			else if (match(">=")) operation = 2;
			else if (matchSingle('<', '<')) operation = 3;
			else if (matchSingle('>', '>')) operation = 4;
			else return true;
			long long right = 0;
			if (!parseShift(right))
				return false;
			value = operation == 1 ? value <= right : operation == 2 ? value >= right
					: operation == 3 ? value < right : value > right;
		}
	}

	bool parseShift(long long& value)
	{
		if (!parseAdditive(value))
			return false;
		for (;;) {
			const bool left = match("<<");
			const bool rightShift = !left && match(">>");
			if (!left && !rightShift)
				return true;
			long long right = 0;
			if (!parseAdditive(right) || right < 0 || right >= 64)
				return false;
			value = left ? value << right : value >> right;
		}
	}

	bool parseAdditive(long long& value)
	{
		if (!parseMultiplicative(value))
			return false;
		for (;;) {
			const bool add = match("+");
			const bool subtract = !add && match("-");
			if (!add && !subtract)
				return true;
			long long right = 0;
			if (!parseMultiplicative(right))
				return false;
			value = add ? value + right : value - right;
		}
	}

	bool parseMultiplicative(long long& value)
	{
		if (!parseUnary(value))
			return false;
		for (;;) {
			int operation = 0;
			if (match("*")) operation = 1;
			else if (match("/")) operation = 2;
			else if (match("%")) operation = 3;
			else return true;
			long long right = 0;
			if (!parseUnary(right) || ((operation == 2 || operation == 3) && right == 0))
				return false;
			value = operation == 1 ? value * right : operation == 2 ? value / right : value % right;
		}
	}

	bool parseUnary(long long& value)
	{
		if (match("!")) {
			if (!parseUnary(value))
				return false;
			value = !value;
			return true;
		}
		if (match("~")) {
			if (!parseUnary(value))
				return false;
			value = ~value;
			return true;
		}
		if (match("+"))
			return parseUnary(value);
		if (match("-")) {
			if (!parseUnary(value))
				return false;
			value = -value;
			return true;
		}
		return parsePrimary(value);
	}

	bool parsePrimary(long long& value)
	{
		if (match("(")) {
			if (!parseLogicalOr(value) || !match(")"))
				return false;
			return true;
		}

		skipWhitespace();
		const size_t begin = position;
		if (position < expression.size() && std::isdigit(static_cast<unsigned char>(expression[position]))) {
			const std::string remaining(expression.substr(position));
			char *end = nullptr;
			value = std::strtoll(remaining.c_str(), &end, 0);
			if (end == remaining.c_str())
				return false;
			position += static_cast<size_t>(end - remaining.c_str());
			while (position < expression.size() && (expression[position] == 'u' || expression[position] == 'U'
					|| expression[position] == 'l' || expression[position] == 'L'))
				++position;
			return true;
		}

		std::string identifier;
		if (!takeIdentifier(identifier)) {
			position = begin;
			return false;
		}
		if (identifier == "defined") {
			const bool parenthesized = match("(");
			std::string name;
			if (!takeIdentifier(name) || (parenthesized && !match(")")))
				return false;
			value = defines.count(name) != 0;
			return true;
		}
		const auto define = defines.find(identifier);
		if (define == defines.end()) {
			value = 0;
			return true;
		}
		if (depth >= 16 || define->second.empty())
			return false;
		long long nested = 0;
		if (!PreprocessorExpressionParser(define->second, defines, depth + 1).evaluateValue(nested))
			return false;
		value = nested;
		return true;
	}

	std::string_view expression;
	const std::unordered_map<std::string, std::string>& defines;
	unsigned depth;
	size_t position = 0;
};

bool evaluateCondition(const std::string& expression, const std::unordered_map<std::string, std::string>& defines,
		bool& result)
{
	return PreprocessorExpressionParser(expression, defines).evaluate(result);
}

std::vector<bool> codeMask(const std::string& line, bool startsInBlockComment, bool *endsInBlockComment = nullptr, bool keepStrings = false)
{
	std::vector<bool> mask(line.size(), false);
	bool inBlockComment = startsInBlockComment;
	for (size_t index = 0; index < line.size();) {
		if (inBlockComment) {
			const size_t end = line.find("*/", index);
			if (end == std::string::npos)
				break;
			index = end + 2;
			inBlockComment = false;
			continue;
		}
		if (index + 1 < line.size() && line[index] == '/' && line[index + 1] == '/')
			break;
		if (index + 1 < line.size() && line[index] == '/' && line[index + 1] == '*') {
			inBlockComment = true;
			index += 2;
			continue;
		}
		if (line[index] == '"' || line[index] == '\'') {
			const size_t begin = index;
			const char quote = line[index++];
			while (index < line.size()) {
				if (line[index] == '\\' && index + 1 < line.size())
					index += 2;
				else if (line[index++] == quote)
					break;
			}
			if (keepStrings) std::fill(mask.begin() + begin, mask.begin() + std::min(index, mask.size()), true);
			continue;
		}
		mask[index++] = true;
	}
	if (endsInBlockComment != nullptr)
		*endsInBlockComment = inBlockComment;
	return mask;
}

void expandObjectMacros(std::string& line, const std::unordered_map<std::string, std::string>& defines,
		bool startsInBlockComment, unsigned maxDepth = 8)
{
	// A macro is disabled while rescanning its replacement. Repeated whole-line
	// passes recursively expanded valid aliases such as OutputSize = vec4(OutputSize, ...).
	std::unordered_set<std::string> expanding;
	const auto expand = [&](const auto& self, const std::string& text, bool inComment, unsigned depth) -> std::string {
		if (depth == 0)
			return text;
		const std::vector<bool> mask = codeMask(text, inComment);
		std::string expanded;
		for (size_t index = 0; index < text.size();) {
			if (!mask[index] || (!std::isalpha(static_cast<unsigned char>(text[index])) && text[index] != '_')) {
				expanded += text[index++];
				continue;
			}
			const size_t begin = index++;
			while (index < text.size() && mask[index]
					&& (std::isalnum(static_cast<unsigned char>(text[index])) || text[index] == '_'))
				++index;
			const std::string name = text.substr(begin, index - begin);
			const auto define = defines.find(name);
			if (define == defines.end() || define->second == name || !expanding.insert(name).second)
				expanded += name;
			else {
				expanded += self(self, define->second, false, depth - 1);
				expanding.erase(name);
			}
		}
		return expanded;
	};
	line = expand(expand, line, startsInBlockComment, maxDepth);
}

struct FunctionMacro {
	std::vector<std::string> arguments;
	std::string body;
};

void expandFunctionMacros(std::string& line, const std::unordered_map<std::string, FunctionMacro>& defines,
		bool startsInBlockComment)
{
	for (unsigned pass = 0; pass < 8; ++pass) {
		bool changed = false;
		for (const auto& [name, macro] : defines) {
			size_t search = 0;
			while ((search = line.find(name, search)) != std::string::npos) {
				const std::vector<bool> mask = codeMask(line, startsInBlockComment);
				if (!mask[search]) {
					search += name.size();
					continue;
				}
				const bool leftBoundary = search == 0 || (!std::isalnum(static_cast<unsigned char>(line[search - 1])) && line[search - 1] != '_');
				const size_t open = line.find_first_not_of(" \t", search + name.size());
				if (!leftBoundary || open == std::string::npos || !mask[open] || line[open] != '(') {
					search += name.size();
					continue;
				}
				int depth = 1;
				size_t close = open + 1;
				for (; close < line.size() && depth != 0; ++close) {
					if (!mask[close]) continue;
					if (line[close] == '(') ++depth;
					else if (line[close] == ')') --depth;
				}
				if (depth != 0) break;
				const size_t closeParenthesis = close - 1;
				std::vector<std::string> arguments;
				size_t begin = open + 1;
				depth = 0;
				for (size_t index = begin; index <= closeParenthesis; ++index) {
					const char character = index < closeParenthesis ? line[index] : ',';
					if (index < closeParenthesis && !mask[index]) continue;
					if (character == '(') ++depth;
					else if (character == ')') --depth;
					if (character == ',' && depth == 0) {
						arguments.push_back(trim(line.substr(begin, index - begin)));
						begin = index + 1;
					}
				}
				if (arguments.size() == 1 && arguments[0].empty()) arguments.clear();
				if (arguments.size() != macro.arguments.size()) {
					search = close + 1;
					continue;
				}
				std::string replacement = macro.body;
				std::unordered_map<std::string, std::string> replacements;
				for (size_t argument = 0; argument < macro.arguments.size(); ++argument)
					replacements.emplace(macro.arguments[argument], arguments[argument]);
				// Substitute parameters once. Rescanning the injected argument would
				// mistake swizzles such as `pos.x` for another use of parameter `x`.
				expandObjectMacros(replacement, replacements, false, 1);
				// close already points one past ')'. Consuming close + 1 also ate the
				// following comma or semicolon from valid shader expressions.
				line.replace(search, close - search, replacement);
				search += replacement.size();
				changed = true;
			}
		}
		if (!changed) break;
	}
}

// Keep the set of disabled macros on each token as replacements are rescanned.
// This permits an object alias to form a function call with the following input,
// while preventing recursive aliases from being expanded again indefinitely.
bool expandShaderMacros(std::string& line, const std::unordered_map<std::string, std::string>& objects,
		const std::unordered_map<std::string, FunctionMacro>& functions, bool startsInBlockComment)
{
	struct MacroToken {
		std::string text;
		bool code;
		bool identifier;
		std::unordered_set<std::string> disabled;
	};
	const auto tokenize = [](const std::string& text, bool inComment) {
		std::vector<MacroToken> tokens;
		const auto mask = codeMask(text, inComment);
		for (size_t i = 0; i < text.size();) {
			const size_t begin = i++;
			const bool identifier = mask[begin] && isIdentifierStart(text[begin]);
			if (identifier)
				while (i < text.size() && mask[i] && isIdentifierPart(text[i])) ++i;
			tokens.push_back({ text.substr(begin, i - begin), mask[begin], identifier, {} });
		}
		return tokens;
	};
	const auto expand = [&](const auto& self, std::vector<MacroToken>& tokens, unsigned depth) -> bool {
		if (depth > 64 || tokens.size() > 1024 * 1024) return false;
		for (size_t i = 0; i < tokens.size();) {
			const MacroToken current = tokens[i];
			if (!current.identifier || current.disabled.count(current.text)) { ++i; continue; }
			const auto object = objects.find(current.text);
			const auto function = functions.find(current.text);
			std::vector<MacroToken> replacement;
			size_t end = i + 1;
			// Function macros also have a sentinel in the object map for #if
			// defined checks; their invocation must take precedence here.
			if (object != objects.end() && function == functions.end())
				replacement = tokenize(object->second, false);
			else if (function != functions.end()) {
				size_t open = end;
				while (open < tokens.size() && tokens[open].code &&
						tokens[open].text.find_first_not_of(" \t") == std::string::npos) ++open;
				if (open == tokens.size() || !tokens[open].code || tokens[open].text != "(") { ++i; continue; }
				std::vector<std::vector<MacroToken>> arguments(1);
				int nesting = 0;
				size_t close = open + 1;
				for (; close < tokens.size(); ++close) {
					const auto& token = tokens[close];
					if (token.code && token.text == ")" && nesting == 0) break;
					if (token.code && token.text == "," && nesting == 0) { arguments.emplace_back(); continue; }
					if (token.code && token.text == "(") ++nesting;
					else if (token.code && token.text == ")") --nesting;
					arguments.back().push_back(token);
				}
				if (close == tokens.size()) { ++i; continue; }
				if (arguments.size() == 1 && std::all_of(arguments[0].begin(), arguments[0].end(),
						[](const MacroToken& t) { return t.code && t.text.find_first_not_of(" \t") == std::string::npos; }))
					arguments.clear();
				if (arguments.size() != function->second.arguments.size()) { ++i; continue; }
				for (auto& argument : arguments)
					if (!self(self, argument, depth + 1)) return false;
				const auto body = tokenize(function->second.body, false);
				for (const auto& token : body) {
					const auto parameter = std::find(function->second.arguments.begin(), function->second.arguments.end(), token.text);
					if (token.identifier && parameter != function->second.arguments.end()) {
						const auto& argument = arguments[parameter - function->second.arguments.begin()];
						replacement.insert(replacement.end(), argument.begin(), argument.end());
					} else replacement.push_back(token);
				}
				end = close + 1;
			} else { ++i; continue; }
			for (auto& token : replacement) {
				token.disabled.insert(current.disabled.begin(), current.disabled.end());
				token.disabled.insert(current.text);
				if (token.disabled.size() > 64) return false;
			}
			if (tokens.size() - (end - i) + replacement.size() > 1024 * 1024) return false;
			if (replacement.size() == end - i)
				std::move(replacement.begin(), replacement.end(), tokens.begin() + i);
			else {
				tokens.erase(tokens.begin() + i, tokens.begin() + end);
				tokens.insert(tokens.begin() + i, replacement.begin(), replacement.end());
			}
		}
		return true;
	};
	auto tokens = tokenize(line, startsInBlockComment);
	if (!expand(expand, tokens, 0)) return false;
	line.clear();
	for (const auto& token : tokens) line += token.text;
	return true;
}

bool preprocess(const std::string& source,
		const std::unordered_map<std::string, std::string>& initialDefines,
		std::string& output, std::string& error, const ShaderIncludeResolver& includeResolver,
		const std::string& sourcePath, bool preserveSource, bool stageScopes)
{
	struct Conditional {
		bool parentActive;
		bool branchTaken;
		bool active;
	};
	std::unordered_map<std::string, std::string> defines = initialDefines;
	std::unordered_map<std::string, FunctionMacro> functionDefines;
	std::unordered_set<std::string> onceFiles;
	std::unordered_map<std::string, std::string> commonDefines;
	std::unordered_map<std::string, FunctionMacro> commonFunctionDefines;
	std::unordered_set<std::string> commonOnceFiles;
	bool stageSeen = false;
	output.clear();
	std::function<bool(const std::string&, const std::string&, unsigned)> process;
	process = [&](const std::string& source, const std::string& path, unsigned depth) {
	if (onceFiles.count(path)) return true;
	if (depth > 16) { error = "Shader include cycle or nesting exceeds 16 files."; return false; }
	std::vector<Conditional> conditionals;
	auto active = [&] { return conditionals.empty() || conditionals.back().active; };
	std::istringstream lines(source);
	std::string line;
	bool inBlockComment = false;
	size_t lineNumber = 0;
	while (std::getline(lines, line)) {
		if (output.size() + line.size() + 1 > 32 * 1024 * 1024) { error = "Expanded shader source exceeds the 32 MiB package limit."; return false; }
		++lineNumber;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		// Splice physical lines before classifying comments or directives. Shader
		// libraries use continued function macros to instantiate typed helpers.
		while (!line.empty() && line.back() == '\\') {
			std::string continuation;
			if (!std::getline(lines, continuation)) {
				error = "line " + std::to_string(lineNumber) + ": unterminated shader line continuation.";
				return false;
			}
			if (output.size() + line.size() + 1 > 32 * 1024 * 1024) { error = "Expanded shader source exceeds the 32 MiB package limit."; return false; }
			++lineNumber;
			if (!continuation.empty() && continuation.back() == '\r')
				continuation.pop_back();
			line.pop_back();
			line += continuation;
		}
		const bool startsInBlockComment = inBlockComment;
		const std::vector<bool> mask = codeMask(line, startsInBlockComment, &inBlockComment);
		size_t directiveStart = 0;
		while (directiveStart < line.size() && (!mask[directiveStart]
				|| std::isspace(static_cast<unsigned char>(line[directiveStart]))))
			++directiveStart;
		if (directiveStart == line.size() || line[directiveStart] != '#') {
			if (active() || preserveSource) {
				if (!preserveSource) {
					if (!expandShaderMacros(line, defines, functionDefines, startsInBlockComment)) {
						error = "Shader macro expansion exceeds the complexity limit.";
						return false;
					}
				}
				if (line.size() + 1 > 32 * 1024 * 1024 - output.size()) {
					error = "Expanded shader source exceeds the 32 MiB package limit.";
					return false;
				}
				output += line + '\n';
			}
			continue;
		}
		// Strip comments for directive parsing without changing quoted include paths.
		const auto directiveMask = codeMask(line, startsInBlockComment, nullptr, true);
		std::string stripped = line.substr(directiveStart);
		for (size_t i = 0; i < stripped.size(); ++i)
			if (!directiveMask[directiveStart + i]) stripped[i] = ' ';
		stripped = trim(stripped);
		std::istringstream directive(stripped.substr(1));
		std::string command;
		directive >> command;
		std::string argument;
		std::getline(directive, argument);
		argument = trim(argument);
		// Include expansion keeps fallback declarations for metadata reflection.
		// The frontend later preprocesses these branches for its selected stage.
		if (preserveSource && (command == "if" || command == "ifdef" || command == "ifndef"
				|| command == "elif" || command == "else" || command == "endif"))
			output += line + '\n';
		if (command == "if" || command == "ifdef" || command == "ifndef") {
			const bool parentActive = active();
			bool condition = false;
			if (!parentActive) condition = false;
			else if (command == "ifdef") condition = defines.count(argument) != 0;
			else if (command == "ifndef") condition = defines.count(argument) == 0;
			else {
				expandFunctionMacros(argument, functionDefines, false);
				if (!evaluateCondition(argument, defines, condition)) {
					error = "line " + std::to_string(lineNumber) + ": unsupported shader #if condition.";
					return false;
				}
			}
			conditionals.push_back({ parentActive, condition, parentActive && condition });
			continue;
		}
		if (command == "elif") {
			if (conditionals.empty()) {
				error = "line " + std::to_string(lineNumber) + ": #elif without #if.";
				return false;
			}
			Conditional& conditional = conditionals.back();
			bool condition = false;
			if (conditional.parentActive && !conditional.branchTaken) {
				expandFunctionMacros(argument, functionDefines, false);
				if (!evaluateCondition(argument, defines, condition)) {
					error = "line " + std::to_string(lineNumber) + ": unsupported shader #elif condition.";
					return false;
				}
			}
			conditional.active = conditional.parentActive && !conditional.branchTaken && condition;
			conditional.branchTaken = conditional.branchTaken || condition;
			continue;
		}
		if (command == "else") {
			if (conditionals.empty()) {
				error = "line " + std::to_string(lineNumber) + ": #else without #if.";
				return false;
			}
			Conditional& conditional = conditionals.back();
			conditional.active = conditional.parentActive && !conditional.branchTaken;
			conditional.branchTaken = true;
			continue;
		}
		if (command == "endif") {
			if (conditionals.empty()) {
				error = "line " + std::to_string(lineNumber) + ": #endif without #if.";
				return false;
			}
			conditionals.pop_back();
			continue;
		}
		if (!active()) {
			if (preserveSource && command != "include") output += line + '\n';
			continue;
		}
		if (command == "include") {
			expandObjectMacros(argument, defines, false);
			const auto includeMask = codeMask(argument, false, nullptr, true);
			// Quoted paths are data; allow a trailing comment but no extra tokens.
			const size_t first = argument.find('"');
			const size_t last = first == std::string::npos ? first : argument.find('"', first + 1);
			bool valid = first == 0 && last != std::string::npos;
			if (valid) for (size_t i = last + 1; i < argument.size(); ++i)
				valid = valid && (!includeMask[i] || std::isspace(static_cast<unsigned char>(argument[i])));
			if (!valid || !includeResolver) { error = "Shader includes require a quoted path and a package resolver."; return false; }
			std::string includedPath, includedText;
			if (!includeResolver(path, argument.substr(first + 1, last - first - 1), includedPath, includedText, error)
					|| !process(includedText, includedPath, depth + 1)) return false;
			continue;
		}
		if (command == "pragma" && argument == "once") {
			onceFiles.insert(path);
			continue;
		}
		if (stageScopes && command == "pragma" && argument.rfind("stage ", 0) == 0) {
			// Slang stages inherit common declarations, not macros from a sibling stage.
			if (!stageSeen) { commonDefines = defines; commonFunctionDefines = functionDefines; commonOnceFiles = onceFiles; stageSeen = true; }
			else { defines = commonDefines; functionDefines = commonFunctionDefines; onceFiles = commonOnceFiles; }
		}
		if (command == "error") { error = argument; return false; }
		if (command == "define") {
			size_t nameEnd = 0;
			while (nameEnd < argument.size() && isIdentifierPart(argument[nameEnd])) ++nameEnd;
			const std::string name = argument.substr(0, nameEnd);
			if (name.empty() || !isIdentifierStart(name.front())) {
				error = "Invalid shader macro name.";
				return false;
			}
			// Function-like macros require an adjacent opening parenthesis, but
			// whitespace is legal between their parameters.
			if (nameEnd < argument.size() && argument[nameEnd] == '(') {
				const size_t close = argument.find(')', nameEnd + 1);
				if (close == std::string::npos) { error = "Unterminated shader macro parameters."; return false; }
				FunctionMacro macro;
				std::istringstream arguments(argument.substr(nameEnd + 1, close - nameEnd - 1));
				for (std::string parameter; std::getline(arguments, parameter, ',');)
					macro.arguments.push_back(trim(parameter));
				if (macro.arguments.size() == 1 && macro.arguments[0].empty()) macro.arguments.clear();
				if (!std::all_of(macro.arguments.begin(), macro.arguments.end(), [](const std::string& parameter) {
					return !parameter.empty() && isIdentifierStart(parameter.front())
							&& std::all_of(parameter.begin() + 1, parameter.end(), isIdentifierPart);
				})) { error = "Invalid function-like shader macro declaration."; return false; }
				macro.body = trim(argument.substr(close + 1));
				functionDefines[name] = std::move(macro);
				defines[name] = name;
			}
			else {
				functionDefines.erase(name);
				defines[name] = trim(argument.substr(nameEnd));
			}
		}
		else if (command == "undef") {
			defines.erase(argument);
			functionDefines.erase(argument);
		}
		output += line + '\n';
	}
	if (!conditionals.empty()) {
		error = "unterminated shader conditional directive.";
		return false;
	}
	return true;
	};
	return process(source, sourcePath, 0);
}

bool lex(const std::string& source, std::vector<Token>& tokens, std::string& error)
{
	size_t index = 0;
	size_t line = 1;
	while (index < source.size()) {
		const char current = source[index];
		if (std::isspace(static_cast<unsigned char>(current))) {
			if (current == '\n')
				++line;
			++index;
			continue;
		}
		if (current == '/' && index + 1 < source.size() && source[index + 1] == '/') {
			index += 2;
			while (index < source.size() && source[index] != '\n')
				++index;
			continue;
		}
		if (current == '/' && index + 1 < source.size() && source[index + 1] == '*') {
			const size_t commentLine = line;
			index += 2;
			while (index + 1 < source.size() && !(source[index] == '*' && source[index + 1] == '/')) {
				if (source[index++] == '\n')
					++line;
			}
			if (index + 1 >= source.size()) {
				error = "line " + std::to_string(commentLine) + ": unterminated block comment.";
				return false;
			}
			index += 2;
			continue;
		}
		const size_t begin = index;
		const size_t tokenLine = line;
		if (current == '#') {
			while (index < source.size() && source[index] != '\n')
				++index;
			const std::string directive = source.substr(begin, index - begin);
			// The preprocessor emits #line between adjacent annotation strings and
			// macro-expanded declaration tokens. It is source bookkeeping, not part
			// of the declaration grammar; keep offsets but omit it from metadata.
			static const std::regex lineDirective(R"(^#\s*line\b)");
			if (!std::regex_search(directive, lineDirective))
				tokens.push_back({ TokenKind::Directive, directive, begin, index, tokenLine });
			continue;
		}
		if (isIdentifierStart(current)) {
			++index;
			while (index < source.size() && isIdentifierPart(source[index]))
				++index;
			tokens.push_back({ TokenKind::Identifier, source.substr(begin, index - begin), begin, index, tokenLine });
			continue;
		}
		if (std::isdigit(static_cast<unsigned char>(current)) || (current == '.' && index + 1 < source.size()
				&& std::isdigit(static_cast<unsigned char>(source[index + 1])))) {
			++index;
			while (index < source.size() && (std::isalnum(static_cast<unsigned char>(source[index])) || source[index] == '.'))
				++index;
			tokens.push_back({ TokenKind::Number, source.substr(begin, index - begin), begin, index, tokenLine });
			continue;
		}
		if (current == '"') {
			++index;
			std::string value;
			bool closed = false;
			while (index < source.size()) {
				char valueChar = source[index++];
				if (valueChar == '\\' && index < source.size()) {
					const char escaped = source[index++];
					switch (escaped) {
					case '0': value += '\0'; break;
					case 'n': value += '\n'; break;
					case 'r': value += '\r'; break;
					case 't': value += '\t'; break;
					case '\\': value += '\\'; break;
					case '"': value += '"'; break;
					default: value += escaped; break;
					}
					continue;
				}
				if (valueChar == '"') {
					closed = true;
					break;
				}
				if (valueChar == '\n')
					++line;
				value += valueChar;
			}
			if (!closed) {
				error = "line " + std::to_string(tokenLine) + ": unterminated string literal.";
				return false;
			}
			tokens.push_back({ TokenKind::String, std::move(value), begin, index, tokenLine });
			continue;
		}
		tokens.push_back({ TokenKind::Symbol, std::string(1, current), begin, ++index, tokenLine });
	}
	tokens.push_back({ TokenKind::End, {}, source.size(), source.size(), line });
	return true;
}

class Parser {
public:
	Parser(const std::string& source, const std::vector<Token>& tokens, ReShadeEffect& effect, std::string& error)
		: source(source), tokens(tokens), effect(effect), error(error) {}

	bool parse()
	{
		while (!isEnd()) {
			if (!parseDeclaration())
				return false;
		}
		selectFirstRunnablePass();
		return true;
	}

private:
	const Token& current() const { return tokens[position]; }
	bool isEnd() const { return current().kind == TokenKind::End; }
	bool isIdentifier(const char *value) const { return current().kind == TokenKind::Identifier && current().text == value; }
	bool isSymbol(const char *value) const { return current().kind == TokenKind::Symbol && current().text == value; }
	bool match(const char *value)
	{
		if (!isSymbol(value))
			return false;
		++position;
		return true;
	}
	bool fail(const std::string& message)
	{
		error = "line " + std::to_string(current().line) + ": " + message;
		return false;
	}
	bool takeIdentifier(std::string& value, const char *description)
	{
		if (current().kind != TokenKind::Identifier)
			return fail(std::string("expected ") + description + ".");
		value = current().text;
		++position;
		return true;
	}
	bool parseDeclaration()
	{
		if (current().kind == TokenKind::Directive) {
			if (isIncludeDirective(current().text))
				return fail("includes are not permitted in ReShade FX effects.");
			effect.preprocessorDirectives.push_back(current().text);
			++position;
			return true;
		}
		if (match(";"))
			return true;
		if (isSymbol("[")) { size_t begin, end; return consumeBalanced("[", "]", begin, end); }
		if (isIdentifier("namespace"))
			return parseNamespace();
		if (isIdentifier("uniform"))
			return parseUniform();
		if (isIdentifier("technique"))
			return parseTechnique();
		if (isIdentifier("struct"))
			return parseStruct();
		return parseTopLevelDeclaration();
	}
	bool parseNamespace()
	{
		++position;
		std::string name;
		if (!takeIdentifier(name, "namespace name") || !match("{"))
			return false;
		while (!isEnd() && !isSymbol("}"))
			if (!parseDeclaration())
				return false;
		if (!match("}"))
			return fail("unterminated namespace.");
		if (isSymbol(";"))
			++position;
		return true;
	}
	bool consumeBalanced(const char *open, const char *close, size_t& begin, size_t& end)
	{
		if (!match(open))
			return fail(std::string("expected '") + open + "'.");
		const size_t openLine = tokens[position - 1].line;
		begin = tokens[position - 1].begin;
		int depth = 1;
		while (!isEnd()) {
			if (match(open)) {
				++depth;
				continue;
			}
			if (match(close)) {
				if (--depth == 0) {
					end = tokens[position - 1].end;
					return true;
				}
				continue;
			}
			++position;
		}
		return fail(std::string("unterminated '") + open + "' block opened on line "
				+ std::to_string(openLine) + ".");
	}
	std::string range(size_t begin, size_t end) const { return source.substr(begin, end - begin); }
	static bool isAnnotationType(const std::string& value)
	{
		const std::string type = lowercase(value);
		return type == "bool" || type == "int" || type == "uint" || type == "float" || type == "string";
	}
	bool parseAnnotations(std::vector<ReShadeAnnotation>& annotations)
	{
		if (!isSymbol("<"))
			return true;
		const size_t annotationBegin = current().begin;
		++position;
		while (!isEnd() && !isSymbol(">")) {
		std::string name;
			if (!takeIdentifier(name, "annotation name"))
				return false;
			// FX annotations may carry an optional scalar type, for example
			// "string source = \"color\"". The type does not change how the
			// render graph interprets the key, so retain only that key.
			if (isAnnotationType(name) && current().kind == TokenKind::Identifier) {
				std::string typedName;
				if (!takeIdentifier(typedName, "annotation name"))
					return false;
				name = std::move(typedName);
			}
			if (!match("="))
				return fail("expected '=' after annotation name.");
			if (isSymbol(";") || isSymbol(">"))
				return fail("expected annotation value.");
			if (current().kind == TokenKind::String) {
				std::string value;
				do {
					value += current().text;
					++position;
				} while (current().kind == TokenKind::String);
				annotations.push_back({ std::move(name), std::move(value) });
				if (isSymbol(";"))
					++position;
				continue;
			}
			const size_t valueBegin = current().begin;
			size_t valueEnd = valueBegin;
			while (!isEnd() && !isSymbol(";") && !isSymbol(">")) {
				valueEnd = current().end;
				++position;
			}
			annotations.push_back({ std::move(name), range(valueBegin, valueEnd) });
			if (isSymbol(";"))
				++position;
		}
		if (!match(">"))
			return fail("unterminated annotation block.");
		effect.annotationRanges.emplace_back(annotationBegin, tokens[position - 1].end);
		return true;
	}
	bool skipArraySuffix()
	{
		while (isSymbol("[")) {
			size_t begin;
			size_t end;
			if (!consumeBalanced("[", "]", begin, end))
				return false;
		}
		return true;
	}
	bool consumeStatement(size_t begin, std::string& definition)
	{
		int braces = 0;
		while (!isEnd()) {
			if (isSymbol("{"))
				++braces;
			else if (isSymbol("}") && braces > 0)
				--braces;
			else if (isSymbol(";") && braces == 0) {
				definition = range(begin, current().end);
				++position;
				return true;
			}
			++position;
		}
		return fail("unterminated declaration.");
	}
	bool parseUniform()
	{
		++position;
		ReShadeUniform uniform;
		if (!takeIdentifier(uniform.typeName, "uniform type") || !takeIdentifier(uniform.name, "uniform name") || !skipArraySuffix()
				|| !parseAnnotations(uniform.annotations))
			return false;
		if (match("=")) {
			if (isSymbol(";"))
				return fail("expected uniform default value.");
			const size_t begin = current().begin;
			size_t end = begin;
			while (!isEnd() && !isSymbol(";")) {
				end = current().end;
				++position;
			}
			uniform.defaultValue = range(begin, end);
		}
		if (!match(";"))
			return fail("expected ';' after uniform declaration.");
		uniform.valueType = valueType(uniform.typeName);
		effect.uniforms.push_back(std::move(uniform));
		return true;
	}
	bool parseStruct()
	{
		++position;
		ReShadeStruct structure;
		if (!takeIdentifier(structure.name, "struct name"))
			return false;
		size_t begin;
		size_t end;
		if (!consumeBalanced("{", "}", begin, end))
			return false;
		structure.body = range(begin + 1, end - 1);
		if (isSymbol(";"))
			++position;
		effect.structs.push_back(std::move(structure));
		return true;
	}
	bool parseTechnique()
	{
		const size_t line = current().line;
		++position;
		ReShadeTechnique technique;
		technique.line = line;
		if (current().kind == TokenKind::Identifier) {
			technique.name = current().text;
			++position;
		}
		std::vector<ReShadeAnnotation> annotations;
		if (!parseAnnotations(annotations))
			return false;
		if (!match("{"))
			return fail("expected technique body.");
		while (!isEnd() && !isSymbol("}")) {
			if (isIdentifier("pass")) {
				ReShadePass pass;
				if (!parsePass(pass))
					return false;
				technique.passes.push_back(std::move(pass));
			}
			else if (!skipUnknownDeclaration()) {
				return false;
			}
		}
		if (!match("}"))
			return fail("unterminated technique.");
		if (isSymbol(";"))
			++position;
		effect.techniques.push_back(std::move(technique));
		return true;
	}
	bool parsePass(ReShadePass& pass)
	{
		pass.line = current().line;
		++position;
		if (current().kind == TokenKind::Identifier) {
			pass.name = current().text;
			++position;
		}
		std::vector<ReShadeAnnotation> annotations;
		if (!parseAnnotations(annotations) || !match("{"))
			return isSymbol("{") ? false : fail("expected pass state block.");
		const size_t bodyBegin = tokens[position - 1].end;
		pass.bodyBegin = bodyBegin;
		while (!isEnd() && !isSymbol("}")) {
			if (isIdentifier("VertexShader") || isIdentifier("PixelShader") || isIdentifier("ComputeShader")) {
				const bool pixelShader = isIdentifier("PixelShader");
				const bool computeShader = isIdentifier("ComputeShader");
				++position;
				if (!match("="))
					return fail("expected '=' in pass shader assignment.");
				std::string entryPoint;
				if (!takeIdentifier(entryPoint, "shader entry point"))
					return false;
				while (match("::")) {
					std::string member;
					if (!takeIdentifier(member, "qualified shader entry point"))
						return false;
					entryPoint += "::" + member;
				}
				// Compute workgroup sizes may be attached to the pass entry point.
				if (isSymbol("<")) {
					size_t begin, end;
					if (!consumeBalanced("<", ">", begin, end))
						return false;
				}
				if (!match(";"))
					return fail("expected ';' after shader entry point.");
				if (computeShader)
					pass.computeShader = std::move(entryPoint);
				else if (pixelShader)
					pass.pixelShader = std::move(entryPoint);
				else
					pass.vertexShader = std::move(entryPoint);
			}
			else if (!skipUnknownDeclaration()) {
				return false;
			}
		}
		if (!isSymbol("}"))
			return fail("unterminated pass state block.");
		pass.stateBlock = range(bodyBegin, current().begin);
		++position;
		if (isSymbol(";"))
			++position;
		return true;
	}
	bool parseTopLevelDeclaration()
	{
		if (current().kind != TokenKind::Identifier)
			return fail("expected a top-level declaration.");
		const size_t declarationBegin = current().begin;
		const std::string typeName = current().text;
		++position;
		if (isResourceType(typeName))
			return parseResource(typeName, declarationBegin);
		std::string name;
		if (!takeIdentifier(name, "declaration name"))
			return false;
		if (isSymbol("("))
			return parseFunction(typeName, name, declarationBegin);
		std::string declaration;
		if (!consumeStatement(declarationBegin, declaration))
			return false;
		effect.globals.push_back(std::move(declaration));
		return true;
	}
	bool parseResource(std::string typeName, size_t declarationBegin)
	{
		if (isSymbol("<")) {
			size_t ignoredBegin;
			size_t typeEnd;
			if (!consumeBalanced("<", ">", ignoredBegin, typeEnd))
				return false;
			typeName = range(declarationBegin, typeEnd);
		}
		std::string name;
		if (!takeIdentifier(name, "resource name") || !skipArraySuffix())
			return false;
		std::vector<ReShadeAnnotation> annotations;
		if (!parseAnnotations(annotations))
			return false;
		std::string definition;
		if (!consumeStatement(declarationBegin, definition))
			return false;
		if (isTextureType(typeName))
			effect.textures.push_back({ std::move(typeName), std::move(name), std::move(definition), std::move(annotations),
					declarationBegin, tokens[position - 1].end });
		else if (lowercase(typeName).find("storage") != std::string::npos)
			effect.storages.push_back({ std::move(typeName), std::move(name), std::move(definition), std::move(annotations), declarationBegin, tokens[position - 1].end });
		else
			effect.samplers.push_back({ std::move(typeName), std::move(name), std::move(definition), std::move(annotations),
					declarationBegin, tokens[position - 1].end });
		return true;
	}
	bool parseFunction(const std::string& returnType, const std::string& name, size_t declarationBegin)
	{
		const size_t declarationLine = current().line;
		size_t ignoredBegin;
		size_t ignoredEnd;
		if (!consumeBalanced("(", ")", ignoredBegin, ignoredEnd))
			return false;
		while (!isEnd() && !isSymbol("{") && !isSymbol(";"))
			++position;
		if (match(";"))
			return true;
		if (!isSymbol("{"))
			return fail("expected function body.");
		const size_t bodyOpen = current().begin;
		size_t ignoredBodyBegin;
		size_t bodyEnd;
		if (!consumeBalanced("{", "}", ignoredBodyBegin, bodyEnd))
			return false;
		effect.functions.push_back({ returnType, name, range(declarationBegin, bodyOpen), range(bodyOpen + 1, bodyEnd - 1),
				declarationLine });
		return true;
	}
	bool skipUnknownDeclaration()
	{
		if (isSymbol("{")) {
			size_t begin;
			size_t end;
			return consumeBalanced("{", "}", begin, end);
		}
		while (!isEnd() && !isSymbol(";") && !isSymbol("}"))
			++position;
		if (match(";"))
			return true;
		return !isEnd();
	}
	void selectFirstRunnablePass()
	{
		for (size_t techniqueIndex = 0; techniqueIndex < effect.techniques.size(); ++techniqueIndex) {
			for (size_t passIndex = 0; passIndex < effect.techniques[techniqueIndex].passes.size(); ++passIndex) {
				if (!effect.techniques[techniqueIndex].passes[passIndex].pixelShader.empty()
						|| !effect.techniques[techniqueIndex].passes[passIndex].computeShader.empty()) {
					effect.selectedTechnique = techniqueIndex;
					effect.selectedPass = passIndex;
					return;
				}
			}
		}
	}
	static ReShadeValueType valueType(const std::string& typeName)
	{
		const std::string type = lowercase(typeName);
		if (type == "bool") return ReShadeValueType::Bool;
		if (type == "int") return ReShadeValueType::Int;
		if (type == "uint") return ReShadeValueType::UInt;
		if (type == "int2") return ReShadeValueType::Int2;
		if (type == "int3") return ReShadeValueType::Int3;
		if (type == "int4") return ReShadeValueType::Int4;
		if (type == "uint2") return ReShadeValueType::UInt2;
		if (type == "uint3") return ReShadeValueType::UInt3;
		if (type == "uint4") return ReShadeValueType::UInt4;
		if (type == "float") return ReShadeValueType::Float;
		if (type == "float2") return ReShadeValueType::Float2;
		if (type == "float3") return ReShadeValueType::Float3;
		if (type == "float4") return ReShadeValueType::Float4;
		if (type == "float2x2") return ReShadeValueType::Float2x2;
		if (type == "float3x3") return ReShadeValueType::Float3x3;
		if (type == "float4x4") return ReShadeValueType::Float4x4;
		return ReShadeValueType::Unknown;
	}
	static bool isTextureType(const std::string& typeName)
	{
		return lowercase(typeName).find("texture") != std::string::npos;
	}
	static bool isResourceType(const std::string& typeName)
	{
		const std::string type = lowercase(typeName);
		return type.find("texture") != std::string::npos || type.find("sampler") != std::string::npos || type.find("storage") != std::string::npos;
	}

	const std::string& source;
	const std::vector<Token>& tokens;
	ReShadeEffect& effect;
	std::string& error;
	size_t position = 0;
};

} // namespace

bool preprocessShaderSource(const std::string& source,
		const std::unordered_map<std::string, std::string>& initialDefines,
		std::string& output, std::string& error, const ShaderIncludeResolver& includeResolver,
		const std::string& sourcePath, bool preserveSource, bool stageScopes)
{
	return preprocess(source, initialDefines, output, error, includeResolver, sourcePath, preserveSource, stageScopes);
}

bool parseReShadeEffect(const std::string& source, ReShadeEffect& effect, std::string& error,
		const ShaderIncludeResolver& includeResolver, const std::string& sourcePath)
{
	error.clear();
	// Advertise the fixed host profile implemented by HollyCast. Buffer values
	// remain expressions so every backend receives the current backbuffer dimensions.
	const std::unordered_map<std::string, std::string> hostDefines {
		{ "__RESHADE__", "60000" },
		{ "__RESHADE_FXC__", "1" },
		// Effects use shader-model-5 semantics before translation to the active API.
		// Feature guards must not discard valid compute entry points as a legacy path.
		{ "__RENDERER__", "0xb000" },
		{ "BUFFER_COLOR_FORMAT", "28" },
		{ "BUFFER_COLOR_SPACE", "1" },
		{ "BUFFER_WIDTH", "FinalViewportSize.x" },
		{ "BUFFER_HEIGHT", "FinalViewportSize.y" },
		{ "BUFFER_RCP_WIDTH", "(1.0 / FinalViewportSize.x)" },
		{ "BUFFER_RCP_HEIGHT", "(1.0 / FinalViewportSize.y)" },
		{ "BUFFER_COLOR_BIT_DEPTH", "8" },
	};
	reshadefx::preprocessor preprocessor;
	// Always intercept includes, including direct parser calls without a package.
	preprocessor.set_include_handler([&](const std::string& parent, const std::string& name,
			std::string& path, std::string& text) {
		if (!includeResolver) { error = "ReShade FX includes require a shader package resolver."; return false; }
		return includeResolver(parent, name, path, text, error);
	});
	for (const auto& define : hostDefines)
		preprocessor.add_macro_definition(define.first, define.second);
	if (!preprocessor.append_string(source + "\n", std::filesystem::u8path(sourcePath))) {
		if (error.empty()) error = preprocessor.errors();
		return false;
	}
	const std::string& preprocessed = preprocessor.output();
	std::vector<Token> tokens;
	if (!lex(preprocessed, tokens, error))
		return false;
	ReShadeEffect parsed;
	Parser parser(preprocessed, tokens, parsed, error);
	if (!parser.parse())
		return false;
	parsed.preprocessedSource = preprocessed;
	effect = std::move(parsed);
	error.clear();
	return true;
}

} // namespace postprocess
