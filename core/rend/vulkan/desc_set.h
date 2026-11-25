/*
	Copyright 2022 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include "vulkan_context.h"
#include <array>
#include <functional>
#include <tuple>

// Frame-local only: descriptor sets can be recycled after the frame completes.
using ImageDescriptorKey = std::tuple<vk::ImageView, vk::Sampler, vk::ImageView, vk::Sampler>;
class ImageDescriptorCache
{
public:
	vk::DescriptorSet find(const ImageDescriptorKey& key) const
	{
		const Entry& entry = entries[index(key)];
		return entry.set && entry.key == key ? entry.set : vk::DescriptorSet{};
	}
	void emplace(const ImageDescriptorKey& key, vk::DescriptorSet set) {
		entries[index(key)] = {key, set};
	}
	void clear() { entries = {}; }
private:
	struct Entry { ImageDescriptorKey key{}; vk::DescriptorSet set{}; };
	static size_t index(const ImageDescriptorKey& key)
	{
		size_t hash = std::hash<VkImageView>{}(static_cast<VkImageView>(std::get<0>(key)));
		hash ^= std::hash<VkSampler>{}(static_cast<VkSampler>(std::get<1>(key))) * 0x9e3779b9u;
		hash ^= std::hash<VkImageView>{}(static_cast<VkImageView>(std::get<2>(key))) * 0x85ebca6bu;
		hash ^= std::hash<VkSampler>{}(static_cast<VkSampler>(std::get<3>(key))) * 0xc2b2ae35u;
		hash ^= hash >> 16;
		hash ^= hash >> 8;
		return hash & 15;
	}
	std::array<Entry, 16> entries{};
};

class DynamicDescSetAlloc
{
public:
	DynamicDescSetAlloc() {
		descSetsInFlight.resize(1);
	}

	void setLayout(vk::DescriptorSetLayout layout) {
		this->layout = layout;
	}
	void setAllocChunk(int size) {
		this->allocChunk = size;
	}

	void nextFrame()
	{
		unsigned swapChainSize = VulkanContext::Instance()->GetSwapChainSize();
		if (swapChainSize > descSetsInFlight.size())
			descSetsInFlight.resize(swapChainSize);
		else
			while (swapChainSize < descSetsInFlight.size())
			{
				for (auto& descset : descSetsInFlight[descSetsInFlight.size() - 1])
					descSets.emplace_back(std::move(descset));
				descSetsInFlight.resize(descSetsInFlight.size() - 1);
			}

		index = (index + 1) % descSetsInFlight.size();
		for (auto& descset : descSetsInFlight[index])
			descSets.emplace_back(std::move(descset));
		descSetsInFlight[index].clear();
	}

	vk::DescriptorSet alloc()
	{
		if (descSets.empty())
		{
			std::vector<vk::DescriptorSetLayout> layouts(allocChunk, layout);
			descSets = VulkanContext::Instance()->GetDevice().allocateDescriptorSetsUnique(
					vk::DescriptorSetAllocateInfo(VulkanContext::Instance()->GetDescriptorPool(), layouts));
		}
		descSetsInFlight[index].emplace_back(std::move(descSets.back()));
		descSets.pop_back();
		return *descSetsInFlight[index].back();
	}

	void term()
	{
		descSets.clear();
		descSetsInFlight.clear();
	}

private:
	vk::DescriptorSetLayout layout;
	std::vector<vk::UniqueDescriptorSet> descSets;
	std::vector<std::vector<vk::UniqueDescriptorSet>> descSetsInFlight;
	int index = 0;
	int allocChunk = 10;
};
