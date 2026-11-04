// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "RenderGraphResources.h"
#include "LocalVertexFactory.h"

/**
 * GPU-written liquid surface mesh shared between the Slosh renderer (which rebuilds it every frame with surface
 * nets, before the scene's base pass) and the surface mesh component's scene proxy (which draws it indirectly
 * through a standard local vertex factory, so any UE material works).
 *
 * Create on the game thread with Create(); every method below is render-thread only. The last reference may be
 * dropped on any thread: deletion is always deferred to the render thread.
 */
class SLOSHRENDER_API FSloshSurfaceMeshBuffers
{
public:
	static TSharedPtr<FSloshSurfaceMeshBuffers, ESPMode::ThreadSafe> Create(uint32 MaxVertices);

	~FSloshSurfaceMeshBuffers();

	/** Allocates the buffers and the vertex factory on first use. */
	void EnsureInitialized(FRHICommandListBase& RHICmdList, ERHIFeatureLevel::Type FeatureLevel);

	/** True once a mesh has been built at least once (until then the proxy draws nothing). */
	bool HasMesh() const { return bHasMesh; }
	void MarkBuilt(uint64 Frame) { bHasMesh = true; LastBuiltFrame = Frame; }
	uint64 GetLastBuiltFrame() const { return LastBuiltFrame; }

	uint32 GetMaxVertices() const { return MaxVertices; }
	uint32 GetMaxIndices() const { return MaxVertices * 6u; }

	const FLocalVertexFactory* GetVertexFactory() const { return VertexFactory.Get(); }
	const FIndexBuffer* GetIndexBuffer() const;
	FRHIBuffer* GetIndirectArgsBuffer() const { return Args.IsValid() ? Args->GetRHI() : nullptr; }

	TRefCountPtr<FRDGPooledBuffer> Positions;   // float x3 per vertex (volume-local cm)
	TRefCountPtr<FRDGPooledBuffer> Tangents;    // RGBA8 SNORM x2 per vertex (TangentX, TangentZ)
	TRefCountPtr<FRDGPooledBuffer> TexCoords;   // float2 per vertex
	TRefCountPtr<FRDGPooledBuffer> Indices;     // uint32
	TRefCountPtr<FRDGPooledBuffer> Args;        // DrawIndexedInstanced arguments

private:
	explicit FSloshSurfaceMeshBuffers(uint32 InMaxVertices);
	void ReleaseRenderResources();

	struct FResources;
	TUniquePtr<FResources> Resources;
	TUniquePtr<FLocalVertexFactory> VertexFactory;
	uint32 MaxVertices = 0;
	uint64 LastBuiltFrame = ~0ull;
	bool bHasMesh = false;
};

using FSloshSurfaceMeshBuffersPtr = TSharedPtr<FSloshSurfaceMeshBuffers, ESPMode::ThreadSafe>;
