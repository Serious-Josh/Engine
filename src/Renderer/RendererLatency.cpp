// NVIDIA Reflex Low Latency: the renderer's side of the frame loop's latency markers (Renderer/Latency/ReflexLatency.h).
// Engine::Frame opens every frame with BeginLatencyFrame before it samples input and marks the start of render submission;
// Render marks the Present it makes. Nothing here records or submits GPU work.

#include "Renderer.h"

void Renderer::BeginLatencyFrame(ReflexMode mode)
{
    // Null before Initialize and after Shutdown, which leaves Reflex inactive.
    m_reflexLatency.BeginFrame(m_graphicsDevice.GetDevice(), mode);
    m_debugUI.SetReflexStatus(m_reflexLatency.GetStatus());
}

void Renderer::MarkLatencyInputSampled()
{
    m_reflexLatency.MarkInputSampled();
}

void Renderer::MarkLatencyRenderSubmitStart()
{
    m_reflexLatency.MarkRenderSubmitStart();
}
