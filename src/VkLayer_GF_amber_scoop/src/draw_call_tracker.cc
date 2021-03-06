// Copyright 2020 The gf-layers Project Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "VkLayer_GF_amber_scoop/draw_call_tracker.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "VkLayer_GF_amber_scoop/amber_scoop_layer.h"
#include "VkLayer_GF_amber_scoop/buffer_copy.h"
#include "VkLayer_GF_amber_scoop/command_buffer_data.h"
#include "VkLayer_GF_amber_scoop/create_info_wrapper.h"
#include "VkLayer_GF_amber_scoop/descriptor_set_data.h"
#include "VkLayer_GF_amber_scoop/graphics_pipeline_data.h"
#include "VkLayer_GF_amber_scoop/vulkan_formats.h"
#include "absl/types/span.h"
#include "gf_layers_layer_util/logging.h"
#include "gf_layers_layer_util/spirv.h"
#include "gf_layers_layer_util/util.h"

#pragma warning(push, 1)  // MSVC: reduces warning level to W1.

#include "spirv-tools/libspirv.h"
#include "spirv-tools/libspirv.hpp"

#pragma warning(pop)

namespace gf_layers::amber_scoop_layer {

namespace {

std::string DisassembleShaderModule(
    const VkShaderModuleCreateInfo& create_info) {
  // Get SPIR-V shader module version.

  // Get a span containing the shader code. |create_info.codeSize| gives the
  // size in bytes, so we convert it to words.
  absl::Span<const uint32_t> code =
      absl::MakeConstSpan(create_info.pCode, create_info.codeSize / 4);

  uint32_t version_word = code[1];

  uint8_t major_version = GetSpirvVersionMajorPart(version_word);
  uint8_t minor_version = GetSpirvVersionMinorPart(version_word);

  if (major_version != 1) {
    LOG("Unknown SPIR-V major version %u", major_version);
    RUNTIME_ASSERT(false);
  }

  spv_target_env target_env;  // NOLINT(cppcoreguidelines-init-variables)
  switch (minor_version) {
    case 0:
      target_env = SPV_ENV_UNIVERSAL_1_0;
      break;
    case 1:
      target_env = SPV_ENV_UNIVERSAL_1_1;
      break;
    case 2:
      target_env = SPV_ENV_UNIVERSAL_1_2;
      break;
    case 3:
      target_env = SPV_ENV_UNIVERSAL_1_3;
      break;
    case 4:
      target_env = SPV_ENV_UNIVERSAL_1_4;
      break;
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    case 5:
      target_env = SPV_ENV_UNIVERSAL_1_5;
      break;
    default:
      LOG("Unknown SPIR-V minor version %u", minor_version);
      RUNTIME_ASSERT(false);
  }

  spvtools::SpirvTools tools(target_env);
  if (!tools.IsValid()) {
    LOG("Failed to instantiate SpirvTools object.");
    RUNTIME_ASSERT(false);
  }

  std::vector<uint32_t> binary(code.begin(), code.end());

  std::string disassembly;
  tools.Disassemble(binary, &disassembly, SPV_BINARY_TO_TEXT_OPTION_INDENT);

  return disassembly;
}

// Creates / opens the file |file_path| and writes the given data |data_span| to
// it. Existing file will be overwritten. Asserts if the file can't be opened.
void WriteDataToFile(const std::string& file_path,
                     const absl::Span<const char>& data_span) {
  // |file_stream| is closed automatically in it's destructor.
  std::ofstream file_stream;
  file_stream.open(file_path,
                   std::ios::out | std::ios::binary | std::ios::trunc);
  // open() doesn't throw exception so we must check that the file is open.
  RUNTIME_ASSERT_MSG(file_stream.is_open(), "Unable to open file: %s",
                     file_path.c_str());

  file_stream.write(data_span.data(),
                    static_cast<std::streamsize>(data_span.size()));
  file_stream.flush();
}

// Returns a buffer/image type name used in Amber's BIND BUFFER/SAMPLER
// command.
const char* GetDescriptorTypeName(VkDescriptorType descriptor_type) {
#pragma GCC diagnostic push  // GCC, clang: disable "-Wswitch-enum" warning.
#pragma GCC diagnostic ignored "-Wswitch-enum"
  // Disable warning: "enumerator 'identifier' in switch of enum 'enumeration'
  // is not explicitly handled by a case label".
#pragma warning(push)
#pragma warning(disable : 4061)
  switch (descriptor_type) {
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      return "combined_image_sampler";
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      return "sampled_image";
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      return "storage";
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      return "storage_dynamic";
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      return "storage_image";
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      return "storage_texel_buffer";
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      return "uniform";
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      return "uniform_dynamic";
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      return "uniform_texel_buffer";
    default:
      RUNTIME_ASSERT_MSG(false, "Unimplemented descriptor type: %i",
                         descriptor_type);
  }
#pragma warning(pop)
#pragma GCC diagnostic pop
}
}  // namespace

void DrawCallTracker::BindGraphicsDescriptorSet(
    uint32_t set_number, VkDescriptorSet descriptor_set,
    const std::vector<uint32_t>& dynamic_offsets,
    uint32_t* dynamic_offset_idx) {
  // Initialize the descriptor set state with an empty map of bindings.
  draw_call_state_.graphics_pipeline_descriptor_sets[set_number] = {};
  DescriptorSetWrapper& descriptor_set_binding =
      draw_call_state_.graphics_pipeline_descriptor_sets[set_number];
  descriptor_set_binding.descriptor_set = descriptor_set;
  descriptor_set_binding.dynamic_offsets = {};

  const DescriptorSetData* descriptor_set_data =
      GetDeviceData()->descriptor_sets.Get(descriptor_set);
  const VkDescriptorSetLayoutCreateInfo& layout_create_info =
      descriptor_set_data->GetDescriptorSetLayoutData().GetCreateInfo();

  // Loop through all bindings in the descriptor set data to check if there's
  // any UNIFORM_BUFFER_DYNAMIC or STORAGE_BUFFER_DYNAMIC descriptors in the
  // set and store the dynamic offsets for them.
  //
  // From the Vulkan spec:
  // If any of the sets being bound include dynamic uniform or storage
  // buffers, then pDynamicOffsets includes one element for each array element
  // in each dynamic descriptor type binding in each set. Values are taken
  // from pDynamicOffsets in an order such that all entries for set N come
  // before set N+1; within a set, entries are ordered by the binding numbers
  // in the descriptor set layouts; and within a binding array, elements are
  // in order. dynamicOffsetCount must equal the total number of dynamic
  // descriptors in the sets being bound.
  for (const std::pair<const BindingNumber,
                       std::vector<VkDescriptorBufferInfo>>& binding :
       *descriptor_set_data->GetDescriptorBufferBindings()) {
    uint32_t binding_number = binding.first;
    const VkDescriptorSetLayoutBinding& layout_binding =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        layout_create_info.pBindings[binding_number];
    if (layout_binding.descriptorType ==
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
        layout_binding.descriptorType ==
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
      for (uint32_t array_element = 0;
           array_element < layout_binding.descriptorCount; array_element++) {
        descriptor_set_binding.dynamic_offsets[binding_number].push_back(
            dynamic_offsets[*dynamic_offset_idx]);
        (*dynamic_offset_idx)++;
      }
    }
  }
}

void DrawCallTracker::HandleDrawCall(uint32_t first_index, uint32_t index_count,
                                     uint32_t first_vertex,
                                     uint32_t vertex_count,
                                     uint32_t first_instance,
                                     uint32_t instance_count) {
  // Silence unused parameter warnings.
  // TODO(ilkkasaa): remove these when they are used.
  (void)first_vertex;
  (void)vertex_count;

  // Graphics pipeline must be bound.
  DEBUG_ASSERT(draw_call_state_.graphics_pipeline);
  DEBUG_ASSERT(draw_call_state_.current_render_pass);

  const uint64_t current_draw_call = global_data_->current_draw_call++;

  // Return if current draw call should not be captured.
  if (current_draw_call < global_data_->settings.start_draw_call ||
      current_draw_call > global_data_->settings.last_draw_call) {
    return;
  }

  DeviceData* device_data =
      global_data_->device_map.Get(DeviceKey(draw_call_state_.queue))->get();

  const VkPipelineShaderStageCreateInfo* vertex_shader = nullptr;
  const VkPipelineShaderStageCreateInfo* fragment_shader = nullptr;
  const GraphicsPipelineData* graphics_pipeline_data =
      device_data->graphics_pipelines.Get(draw_call_state_.graphics_pipeline)
          ->get();

  for (const VkPipelineShaderStageCreateInfo& stage_create_info :
       absl::MakeConstSpan(
           graphics_pipeline_data->GetCreateInfo().pStages,
           graphics_pipeline_data->GetCreateInfo().stageCount)) {
    if (stage_create_info.stage == VK_SHADER_STAGE_VERTEX_BIT) {
      vertex_shader = &stage_create_info;
    } else if (stage_create_info.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
      fragment_shader = &stage_create_info;
    } else {
      LOG("Shader stage not handled.");
      RUNTIME_ASSERT(false);
    }
  }
  // Both vertex and fragment shaders are required.
  if (vertex_shader == nullptr || fragment_shader == nullptr) {
    LOG("Missing vertex or fragment shader.");
    RUNTIME_ASSERT(false);
  }

  // Initialize string streams for different parts of the amber file.
  std::ostringstream buffer_declaration_str;
  std::ostringstream pipeline_str;
  // TODO(ilkkasaa): add other string streams

  // Pipeline
  pipeline_str << "PIPELINE graphics pipeline" << std::endl;
  pipeline_str << "  ATTACH vertex_shader" << std::endl;
  pipeline_str << "  ATTACH fragment_shader" << std::endl;

  // Highest index value (if indices are used). Updated in
  // |CreateIndexBufferDeclarations| function.
  uint32_t max_index_value = 0;

  if (index_count > 0) {
    CreateIndexBufferDeclarations(device_data, index_count, &max_index_value,
                                  buffer_declaration_str, pipeline_str);
  }

  CreateVertexBufferDeclarations(device_data, buffer_declaration_str,
                                 pipeline_str);

  CreateDescriptorSetDeclarations(device_data, buffer_declaration_str,
                                  pipeline_str);

  // Add frame buffer that can be exported to PNG.
  buffer_declaration_str << "BUFFER framebuffer FORMAT B8G8R8A8_UNORM"
                         << std::endl
                         << std::endl;
  pipeline_str << "  BIND BUFFER framebuffer AS color LOCATION 0" << std::endl;

  // End pipeline
  pipeline_str << "END" << std::endl << std::endl;

  std::string amber_file_name = global_data_->settings.output_file_prefix +
                                "_" + std::to_string(current_draw_call) +
                                ".amber";

  // Start generating the Amber file.
  std::ofstream amber_file;
  amber_file.open(amber_file_name, std::ios::trunc | std::ios::out);

  // Add shader modules.
  amber_file << "#!amber" << std::endl << std::endl;
  amber_file << "SHADER vertex vertex_shader SPIRV-ASM" << std::endl;
  amber_file << DisassembleShaderModule(
                    graphics_pipeline_data
                        ->GetShaderModuleData(vertex_shader->module)
                        ->GetCreateInfo())
             << std::endl;
  amber_file << "END" << std::endl << std::endl;
  amber_file << "SHADER fragment fragment_shader SPIRV-ASM" << std::endl;
  amber_file << DisassembleShaderModule(
                    graphics_pipeline_data
                        ->GetShaderModuleData(fragment_shader->module)
                        ->GetCreateInfo())
             << std::endl;
  amber_file << "END" << std::endl << std::endl;

  // Append string streams to the Amber file.
  amber_file << buffer_declaration_str.str();
  amber_file << pipeline_str.str();

  // TODO(ilkkasaa): get primitive topology from VkGraphicsPipelineCreateInfo
  const std::string topology = "TRIANGLE_LIST";

  // Add run commands.
  if (index_count > 0) {
    amber_file << "RUN pipeline DRAW_ARRAY AS " << topology
               << " INDEXED START_IDX " << first_index << " COUNT "
               << index_count;
  } else {
    amber_file << "RUN pipeline DRAW_ARRAY AS " << topology;
  }
  if (instance_count > 0) {
    amber_file << " START_INSTANCE " << first_instance << " INSTANCE_COUNT "
               << instance_count;
  }
  amber_file << std::endl;

  amber_file.close();
}

void DrawCallTracker::CreateDescriptorSetDeclarations(
    DeviceData* device_data, std::ostringstream& buffer_declaration_str,
    std::ostringstream& pipeline_str) {
  // Keep list of copied and saved buffers to avoid copying same buffer multiple
  // times. Key is the Vulkan buffer handle and value is the name of the buffer.
  std::unordered_map<VkBuffer, std::string> copied_buffers;

  // Loop through all descriptor set bindings. Create buffer declarations for
  // all descriptors and store the data to binary files.
  for (const std::pair<const DescriptorSetNumber, DescriptorSetWrapper>&
           descriptor_set_binding :
       draw_call_state_.graphics_pipeline_descriptor_sets) {
    // Use some variables to get easier access to the resources and to clarify
    // the names.
    const uint32_t descriptor_set_number = descriptor_set_binding.first;
    DescriptorSetData* descriptor_set_data = device_data->descriptor_sets.Get(
        descriptor_set_binding.second.descriptor_set);

    // Loop through all uniform / storage buffer descriptors within the set.
    // Copy the buffers used by the descriptors and store the contents to files.
    for (const std::pair<const DescriptorSetData::BindingNumber,
                         std::vector<VkDescriptorBufferInfo>>& buffer_binding :
         *descriptor_set_data->GetDescriptorBufferBindings()) {
      std::stringstream descriptor_range_string;
      std::stringstream descriptor_offset_string;
      const uint32_t binding_number = buffer_binding.first;

      // Get amount of descriptors in the current binding.
      uint32_t descriptor_count =
          // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
          descriptor_set_data->GetDescriptorSetLayoutData()
              .GetCreateInfo()
              .pBindings[binding_number]
              .descriptorCount;
      // List of buffer names. One buffer name per array element.
      std::vector<std::string> buffer_names(descriptor_count);

      const auto& descriptor_set_layout =
          descriptor_set_data->GetDescriptorSetLayoutData()
              .GetCreateInfo()
              .pBindings;

      // Initialize dynamic offsets with zeroes. Initial values are used if the
      // descriptor set doesn't use dynamic offsets.
      std::vector<DynamicOffset> dynamic_offsets(descriptor_count, 0);
      // Create dynamic offset bind string(s) and store the dynamic offset
      // values to the vector defined above.
      std::stringstream dynamic_offset_string;
      if (descriptor_set_layout->descriptorType ==
              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
          descriptor_set_layout->descriptorType ==
              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
        dynamic_offset_string << " OFFSET";
        dynamic_offsets =
            descriptor_set_binding.second.dynamic_offsets.at(binding_number);
        // Loop through all dynamic offsets of the current binding.
        for (DynamicOffset offset : dynamic_offsets) {
          dynamic_offset_string << " " << offset;
        }
      }

      // Loop through all descriptors (array elements) of this binding. Copy the
      // contents of the buffer used by the descriptor to a binary file if it's
      // not already copied and create buffer declaration string. Append the
      // descriptor offset and range values to the BIND BUFFER command.
      for (uint32_t array_element = 0; array_element < descriptor_count;
           array_element++) {
        const VkDescriptorBufferInfo& buffer_info =
            buffer_binding.second[array_element];

        const VkBufferCreateInfo& buffer_create_info =
            device_data->buffers.Get(buffer_info.buffer)->GetCreateInfo();

        // Check if the buffer has been copied already, i.e. it is stored in the
        // |copied_buffers| map. If not, copy the buffer and store its contents
        // to a file.
        std::string buffer_name;
        if (copied_buffers.count(buffer_info.buffer) == 0) {
          // Generate name for the buffer.
          std::ostringstream name_string_stream;
          name_string_stream << "descriptor_" << descriptor_set_number << "_"
                             << binding_number << "_" << array_element;
          buffer_name = name_string_stream.str();

          DEBUG_ASSERT((buffer_create_info.usage &
                        (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) != 0);

          // Generate the file name.
          std::string buffer_file_name =
              global_data_->settings.output_file_prefix;
          buffer_file_name.append("_").append(buffer_name).append(".bin");

          // Copy the buffer to a host visible memory.
          std::unique_ptr<BufferCopy> descriptor_buffer_copy =
              std::make_unique<BufferCopy>(
                  device_data, buffer_info.buffer, buffer_create_info.size,
                  draw_call_state_.queue, GetCommandPool(device_data));

          // Write the buffer contents to a file.
          WriteDataToFile(buffer_file_name,
                          descriptor_buffer_copy->GetCopiedData());

          // Store the buffer name to the |copied_buffers| map.
          copied_buffers.emplace(buffer_info.buffer, buffer_name);

          // Create buffer declaration.
          buffer_declaration_str
              << "BUFFER " << buffer_name << " DATA_TYPE R8_UINT SIZE "
              << buffer_create_info.size << " FILE BINARY " << buffer_file_name
              << std::endl;
        } else {
          // Buffer is already copied. Get the buffer name from the map.
          buffer_name = copied_buffers.at(buffer_info.buffer);
        }

        VkDeviceSize buffer_range = buffer_info.range;
        // We can't use VK_WHOLE_SIZE as range, so we need to compute the actual
        // range, ie. end of the buffer - offset - dynamic offset. Note: if
        // dynamic offsets aren't used, the |dynamic_offsets| vector contains
        // only zeroes, so it has no effect.
        if (buffer_range == VK_WHOLE_SIZE) {
          buffer_range = buffer_create_info.size - buffer_info.offset -
                         dynamic_offsets[array_element];
        }
        descriptor_offset_string << " " << buffer_info.offset;
        descriptor_range_string << " " << buffer_range;
        buffer_names[array_element] = buffer_name;
      }

      pipeline_str << "  BIND ";
      // Single descriptors are bound using "BIND BUFFER" command and descriptor
      // arrays are bound using "BIND BUFFER_ARRAY" command.
      pipeline_str << (descriptor_count == 1 ? "BUFFER" : "BUFFER_ARRAY");
      // Add buffer names (one per array element).
      for (const std::string& buffer_name : buffer_names) {
        pipeline_str << " " << buffer_name;
      }
      pipeline_str << " AS "
                   << GetDescriptorTypeName(
                          descriptor_set_layout->descriptorType)
                   << " DESCRIPTOR_SET " << descriptor_set_number << " BINDING "
                   << binding_number;

      // Add descriptor buffer range and offsets.
      pipeline_str << dynamic_offset_string.str();
      pipeline_str << " DESCRIPTOR_OFFSET" << descriptor_offset_string.str();
      pipeline_str << " DESCRIPTOR_RANGE" << descriptor_range_string.str();
      pipeline_str << std::endl;
    }

    // TODO(ilkkasaa): implement images and samplers here.
  }
}

void DrawCallTracker::CreateIndexBufferDeclarations(
    DeviceData* device_data, uint32_t index_count, uint32_t* max_index_value,
    std::ostringstream& declaration_string_stream,
    std::ostringstream& pipeline_string_stream) const {
  VkBuffer index_buffer = draw_call_state_.bound_index_buffer.buffer;
  BufferData* buffer_create_info = device_data->buffers.Get(index_buffer);

  // Copy the buffer.
  const VkDeviceSize& index_buffer_size =
      buffer_create_info->GetCreateInfo().size;
  auto index_buffer_copy =
      BufferCopy(device_data, index_buffer, index_buffer_size,
                 draw_call_state_.queue, GetCommandPool(device_data));

  // Create index buffer declaration string.

  // Amber supports only 32-bit indices. 16-bit indices will be used as
  // 32-bit.
  declaration_string_stream << "BUFFER index_buffer DATA_TYPE uint32 ";
  declaration_string_stream << "DATA " << std::endl << "  ";

  // Index data starts from the buffer beginning + offset.
  const char* first_index_address =
      &index_buffer_copy
           .GetCopiedData()[draw_call_state_.bound_index_buffer.offset];

  if (draw_call_state_.bound_index_buffer.index_type == VK_INDEX_TYPE_UINT16) {
    // 16-bit indices
    // Create a new span with type of |uint16_t| so the indices can be read
    // easily.
    absl::Span<const uint16_t> index_data = absl::MakeConstSpan(
        reinterpret_cast<const uint16_t*>(first_index_address), index_count);
    // Append values to the index buffer string.
    for (uint16_t index : index_data) {
      *max_index_value =
          std::max(*max_index_value, static_cast<uint32_t>(index));
      declaration_string_stream << index << " ";
    }
  } else if (draw_call_state_.bound_index_buffer.index_type ==
             VK_INDEX_TYPE_UINT32) {
    // 32-bit indices
    // Create a new span with type of |uint32_t| so the indices can be read
    // easily.
    absl::Span<const uint32_t> index_data = absl::MakeConstSpan(
        reinterpret_cast<const uint32_t*>(first_index_address), index_count);
    // Append values to the index buffer string.
    for (uint32_t index : index_data) {
      *max_index_value = std::max(*max_index_value, index);
      declaration_string_stream << index << " ";
    }
  } else {
    LOG("Index type not supported.");
    RUNTIME_ASSERT(false);
  }
  declaration_string_stream << std::endl << "END" << std::endl << std::endl;

  // Use indices in the pipeline.
  pipeline_string_stream << "  INDEX_DATA index_buffer" << std::endl;
}

void DrawCallTracker::CreateVertexBufferDeclarations(
    DeviceData* device_data, std::ostringstream& buffer_declaration_str,
    std::ostringstream& pipeline_str) {
  bool vertex_buffer_found = false;

  const VkGraphicsPipelineCreateInfo& graphics_pipeline_create_info =
      device_data->graphics_pipelines.Get(draw_call_state_.graphics_pipeline)
          ->get()
          ->GetCreateInfo();

  const VkPipelineVertexInputStateCreateInfo& vertex_input_state =
      *graphics_pipeline_create_info.pVertexInputState;

  // Make span for attribute and binding descriptions for safer access.
  absl::Span<const VkVertexInputAttributeDescription> attribute_descriptions =
      absl::MakeConstSpan<>(vertex_input_state.pVertexAttributeDescriptions,
                            vertex_input_state.vertexAttributeDescriptionCount);
  absl::Span<const VkVertexInputBindingDescription> binding_descriptions =
      absl::MakeConstSpan<>(vertex_input_state.pVertexBindingDescriptions,
                            vertex_input_state.vertexBindingDescriptionCount);

  // Keep list of copied and saved buffers to avoid copying same buffer multiple
  // times. Key is the Vulkan buffer handle and value is the name of the buffer.
  std::unordered_map<VkBuffer, std::string> copied_buffers;

  // Go through all attribute descriptions to get the pipeline's vertex buffer
  // bindings. First copy the whole |VkBuffer| into a host visible memory.
  // Copied buffers are stored temporarily in |copied_buffers| map to prevent
  // the same buffer being copied multiple times if it's used in more than one
  // binding.
  for (const auto& attribute_description : attribute_descriptions) {
    // Find description for the binding used in this vertex attribute.
    const auto* binding_description = std::find_if(
        binding_descriptions.begin(), binding_descriptions.end(),
        [&](const VkVertexInputBindingDescription& _binding_description) {
          return _binding_description.binding == attribute_description.binding;
        });

    // Verify that a binding description exists.
    RUNTIME_ASSERT_MSG(
        binding_description != binding_descriptions.end(),
        "Unable to find |VkVertexInputBindingDescription| for binding [%i] "
        "in |VkVertexInputAttributeDescription| at location [%i]",
        attribute_description.binding, attribute_description.location);

    // Get Vulkan handle and create info for the buffer.
    const VertexBufferBinding& vertex_buffer =
        draw_call_state_.bound_vertex_buffers.at(attribute_description.binding);
    const VkBufferCreateInfo& buffer_create_info =
        device_data->buffers.Get(vertex_buffer.buffer)->GetCreateInfo();

    // Usage bits should contain |VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|
    DEBUG_ASSERT(
        (buffer_create_info.usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0);

    // Check if the buffer has been copied already, i.e. it is stored in the
    // |copied_buffers| map. If not, copy the buffer and store its contents to a
    // file.
    std::string buffer_name;
    if (copied_buffers.count(vertex_buffer.buffer) == 0) {
      // Generate unique name for the buffer.
      const auto buffer_order = static_cast<uint32_t>(copied_buffers.size());
      buffer_name = "vert_";
      buffer_name.append(std::to_string(buffer_order));

      // Generate the file name.
      std::string buffer_file_name = global_data_->settings.output_file_prefix;
      buffer_file_name.append("_").append(buffer_name).append(".bin");

      // Copy the buffer to a host visible memory.
      std::unique_ptr<BufferCopy> vertex_buffer_copy =
          std::make_unique<BufferCopy>(
              device_data, vertex_buffer.buffer, buffer_create_info.size,
              draw_call_state_.queue, GetCommandPool(device_data));

      // Write the buffer contents to a file.
      WriteDataToFile(buffer_file_name, vertex_buffer_copy->GetCopiedData());

      // Store the buffer name to the |copied_buffers| map.
      copied_buffers.emplace(vertex_buffer.buffer, buffer_name);

      // Create buffer declaration.
      buffer_declaration_str
          << "BUFFER " << buffer_name << " DATA_TYPE R8_UINT SIZE "
          << buffer_create_info.size << " FILE BINARY " << buffer_file_name
          << std::endl;
    } else {
      // Buffer is already copied. Get the buffer name from the map.
      buffer_name = copied_buffers.at(vertex_buffer.buffer);
    }

    std::string input_rate_str;
    if (binding_description->inputRate == VK_VERTEX_INPUT_RATE_VERTEX) {
      input_rate_str = "vertex";
    } else if (binding_description->inputRate ==
               VK_VERTEX_INPUT_RATE_INSTANCE) {
      input_rate_str = "instance";
    } else {
      RUNTIME_ASSERT_MSG(false, "Invalid vertex input rate: %i",
                         binding_description->inputRate);
    }

    const VertexBufferBinding& vertex_buffer_binding =
        draw_call_state_.bound_vertex_buffers.at(attribute_description.binding);

    const char* format_name =
        VkFormatToAmberFormatName(attribute_description.format);

    pipeline_str << "  VERTEX_DATA " << buffer_name << " LOCATION "
                 << attribute_description.location << " RATE " << input_rate_str
                 << " FORMAT " << format_name << " OFFSET "
                 << (attribute_description.offset +
                     vertex_buffer_binding.offset)
                 << " STRIDE " << std::to_string(binding_description->stride)
                 << std::endl;

    vertex_buffer_found = true;
  }
  RUNTIME_ASSERT_MSG(vertex_buffer_found,
                     "Vertex buffer not found. Unable to create an Amber file, "
                     "because Amber requires at least one vertex buffer.");
}

VkCommandPool DrawCallTracker::GetCommandPool(DeviceData* device_data) const {
  CommandBufferData* command_buffer_data =
      device_data->command_buffers_data.Get(draw_call_state_.command_buffer);
  return command_buffer_data->GetAllocateInfo()->commandPool;
}

}  // namespace gf_layers::amber_scoop_layer
