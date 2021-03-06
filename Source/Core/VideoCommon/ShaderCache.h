// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/LinearDiskCache.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AsyncShaderCompiler.h"
#include "VideoCommon/GXPipelineTypes.h"
#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/UberShaderPixel.h"
#include "VideoCommon/UberShaderVertex.h"
#include "VideoCommon/VertexShaderGen.h"

class NativeVertexFormat;

namespace VideoCommon
{
class ShaderCache final
{
public:
  ShaderCache();
  ~ShaderCache();

  // Perform at startup, create descriptor layouts, compiles all static shaders.
  bool Initialize();
  void Shutdown();

  // Changes the shader host config. Shaders will be reloaded if there are changes.
  void SetHostConfig(const ShaderHostConfig& host_config, u32 efb_multisamples);

  // Reloads/recreates all shaders and pipelines.
  void Reload();

  // Retrieves all pending shaders/pipelines from the async compiler.
  void RetrieveAsyncShaders();

  // Get utility shader header based on current config.
  std::string GetUtilityShaderHeader() const;

  // Accesses ShaderGen shader caches
  const AbstractPipeline* GetPipelineForUid(const GXPipelineUid& uid);
  const AbstractPipeline* GetUberPipelineForUid(const GXUberPipelineUid& uid);

  // Accesses ShaderGen shader caches asynchronously.
  // The optional will be empty if this pipeline is now background compiling.
  std::optional<const AbstractPipeline*> GetPipelineForUidAsync(const GXPipelineUid& uid);

private:
  void WaitForAsyncCompiler();
  void LoadShaderCaches();
  void ClearShaderCaches();
  void LoadPipelineUIDCache();
  void CompileMissingPipelines();
  void InvalidateCachedPipelines();
  void ClearPipelineCaches();
  void PrecompileUberShaders();

  // GX shader compiler methods
  std::unique_ptr<AbstractShader> CompileVertexShader(const VertexShaderUid& uid) const;
  std::unique_ptr<AbstractShader>
  CompileVertexUberShader(const UberShader::VertexShaderUid& uid) const;
  std::unique_ptr<AbstractShader> CompilePixelShader(const PixelShaderUid& uid) const;
  std::unique_ptr<AbstractShader>
  CompilePixelUberShader(const UberShader::PixelShaderUid& uid) const;
  const AbstractShader* InsertVertexShader(const VertexShaderUid& uid,
                                           std::unique_ptr<AbstractShader> shader);
  const AbstractShader* InsertVertexUberShader(const UberShader::VertexShaderUid& uid,
                                               std::unique_ptr<AbstractShader> shader);
  const AbstractShader* InsertPixelShader(const PixelShaderUid& uid,
                                          std::unique_ptr<AbstractShader> shader);
  const AbstractShader* InsertPixelUberShader(const UberShader::PixelShaderUid& uid,
                                              std::unique_ptr<AbstractShader> shader);
  const AbstractShader* CreateGeometryShader(const GeometryShaderUid& uid);
  bool NeedsGeometryShader(const GeometryShaderUid& uid) const;

  // GX pipeline compiler methods
  AbstractPipelineConfig
  GetGXPipelineConfig(const NativeVertexFormat* vertex_format, const AbstractShader* vertex_shader,
                      const AbstractShader* geometry_shader, const AbstractShader* pixel_shader,
                      const RasterizationState& rasterization_state, const DepthState& depth_state,
                      const BlendingState& blending_state);
  std::optional<AbstractPipelineConfig> GetGXPipelineConfig(const GXPipelineUid& uid);
  std::optional<AbstractPipelineConfig> GetGXUberPipelineConfig(const GXUberPipelineUid& uid);
  const AbstractPipeline* InsertGXPipeline(const GXPipelineUid& config,
                                           std::unique_ptr<AbstractPipeline> pipeline);
  const AbstractPipeline* InsertGXUberPipeline(const GXUberPipelineUid& config,
                                               std::unique_ptr<AbstractPipeline> pipeline);
  void AppendGXPipelineUID(const GXPipelineUid& config);

  // ASync Compiler Methods
  void QueueVertexShaderCompile(const VertexShaderUid& uid);
  void QueueVertexUberShaderCompile(const UberShader::VertexShaderUid& uid);
  void QueuePixelShaderCompile(const PixelShaderUid& uid);
  void QueuePixelUberShaderCompile(const UberShader::PixelShaderUid& uid);
  void QueuePipelineCompile(const GXPipelineUid& uid);
  void QueueUberPipelineCompile(const GXUberPipelineUid& uid);

  // Configuration bits.
  APIType m_api_type = APIType::Nothing;
  ShaderHostConfig m_host_config = {};
  u32 m_efb_multisamples = 1;
  std::unique_ptr<AsyncShaderCompiler> m_async_shader_compiler;

  // GX Shader Caches
  template <typename Uid>
  struct ShaderModuleCache
  {
    struct Shader
    {
      std::unique_ptr<AbstractShader> shader;
      bool pending;
    };
    std::map<Uid, Shader> shader_map;
    LinearDiskCache<Uid, u8> disk_cache;
  };
  ShaderModuleCache<VertexShaderUid> m_vs_cache;
  ShaderModuleCache<GeometryShaderUid> m_gs_cache;
  ShaderModuleCache<PixelShaderUid> m_ps_cache;
  ShaderModuleCache<UberShader::VertexShaderUid> m_uber_vs_cache;
  ShaderModuleCache<UberShader::PixelShaderUid> m_uber_ps_cache;

  // GX Pipeline Caches - .first - pipeline, .second - pending
  std::map<GXPipelineUid, std::pair<std::unique_ptr<AbstractPipeline>, bool>> m_gx_pipeline_cache;
  std::map<GXUberPipelineUid, std::pair<std::unique_ptr<AbstractPipeline>, bool>>
      m_gx_uber_pipeline_cache;

  // Disk cache of pipeline UIDs
  // We can't use the whole UID as a type
  struct GXPipelineDiskCacheUid
  {
    PortableVertexDeclaration vertex_decl;
    VertexShaderUid vs_uid;
    GeometryShaderUid gs_uid;
    PixelShaderUid ps_uid;
    u32 rasterization_state_bits;
    u32 depth_state_bits;
    u32 blending_state_bits;
  };
  LinearDiskCache<GXPipelineDiskCacheUid, u8> m_gx_pipeline_uid_disk_cache;
};

}  // namespace VideoCommon

extern std::unique_ptr<VideoCommon::ShaderCache> g_shader_cache;
