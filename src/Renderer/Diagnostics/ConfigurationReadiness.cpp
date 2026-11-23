#include "ConfigurationReadiness.h"

#include "Core/Log.h"

#include <cstdio>

namespace
{
    const char* ModeName(RendererLightingMode mode)
    {
        switch(mode)
        {
        case RendererLightingMode::Legacy:
            return "Ray Tracing (Legacy)";
        case RendererLightingMode::PathTracedReference:
            return "Reference";
        case RendererLightingMode::PathTracedRasterPrimary:
            return "Raster Primary";
        case RendererLightingMode::PathTracedRasterPrimaryReSTIRDI:
            return "Path Tracing (ReSTIR DI)";
        case RendererLightingMode::PathTracedRasterPrimaryReSTIRPT:
            return "Path Tracing (ReSTIR PT Enhanced)";
        }

        return "?";
    }

    const char* DenoiserName(PathTraceDenoiserMode denoiser)
    {
        switch(denoiser)
        {
        case PathTraceDenoiserMode::Off:
            return "Off";
        case PathTraceDenoiserMode::Engine:
            return "Engine";
        case PathTraceDenoiserMode::NrdReblur:
            return "REBLUR";
        case PathTraceDenoiserMode::NrdRelax:
            return "RELAX";
        case PathTraceDenoiserMode::NrdHybrid:
            return "hybrid";
        }

        return "?";
    }

    bool PathTraced(RendererLightingMode mode)
    {
        return mode != RendererLightingMode::Legacy;
    }
}

const char* ReadinessStateName(ReadinessState state) noexcept
{
    switch(state)
    {
    case ReadinessState::Ready:
        return "ready";
    case ReadinessState::Preparing:
        return "preparing";
    case ReadinessState::Unsupported:
        return "unsupported";
    case ReadinessState::Failed:
        return "failed";
    case ReadinessState::NotApplicable:
        return "n/a";
    }

    return "?";
}

const char* ReadinessDependencyName(ReadinessDependency dependency) noexcept
{
    switch(dependency)
    {
    case ReadinessDependency::PathTracingAvailability:
        return "path tracing availability";
    case ReadinessDependency::PathTracingPipelines:
        return "path tracing pipelines";
    case ReadinessDependency::PathTracingResources:
        return "path tracing resources";
    case ReadinessDependency::DenoiserPipelines:
        return "denoiser pipelines";
    case ReadinessDependency::DenoiserInstance:
        return "denoiser instance";
    case ReadinessDependency::Psr:
        return "PSR";
    case ReadinessDependency::Sharc:
        return "SHARC";
    case ReadinessDependency::Dlss:
        return "DLSS";
    case ReadinessDependency::RenderExtent:
        return "render extent";
    case ReadinessDependency::RayTracingResources:
        return "ray tracing resources";
    case ReadinessDependency::Count:
        break;
    }

    return "?";
}

std::string DescribeRequirements(const ConfigurationRequirements& requirements)
{
    char text[256];
    std::snprintf(text, sizeof(text), "%s, denoiser %s, PSR %u, SHARC %s, %s, scale %d (DLSS %s), output %ux%u",
        ModeName(requirements.lightingMode), PathTraced(requirements.lightingMode) ? DenoiserName(requirements.denoiser) : "-",
        requirements.psrChain, requirements.sharc ? "on" : "off", requirements.nrdReflections ? "NRD reflections" : "native reflections",
        requirements.renderScaleIndex, requirements.dlss ? "on" : "off", requirements.outputWidth, requirements.outputHeight);
    return text;
}

std::string DescribeRendered(const RenderedConfiguration& rendered)
{
    if(!rendered.lit)
    {
        char unlit[160];
        std::snprintf(unlit, sizeof(unlit), "nothing lit (%s), render %ux%u, output %ux%u",
            rendered.lightingMode == RendererLightingMode::Legacy ? "startup preparation" : "no resident geometry",
            rendered.renderWidth, rendered.renderHeight, rendered.outputWidth, rendered.outputHeight);
        return unlit;
    }

    char text[256];
    std::snprintf(text, sizeof(text), "%s, denoiser %s, PSR %u, SHARC %s, DLSS %s, render %ux%u, output %ux%u",
        ModeName(rendered.lightingMode), PathTraced(rendered.lightingMode) ? DenoiserName(rendered.denoiser) : "-", rendered.psrChain,
        rendered.sharc ? "on" : "off", rendered.dlss ? "on" : "off", rendered.renderWidth, rendered.renderHeight, rendered.outputWidth,
        rendered.outputHeight);
    return text;
}

bool RenderedMatchesRequest(const RenderedConfiguration& rendered, const ConfigurationRequirements& requirements)
{
    if(!rendered.lit || rendered.lightingMode != requirements.lightingMode || rendered.outputWidth != requirements.outputWidth ||
        rendered.outputHeight != requirements.outputHeight)
    {
        return false;
    }

    if(!PathTraced(requirements.lightingMode))
    {
        return true;
    }

    return rendered.denoiser == requirements.denoiser && rendered.psrChain == requirements.psrChain &&
        rendered.sharc == requirements.sharc && rendered.dlss == requirements.dlss;
}

double ConfigurationReadinessTrace::Since(Clock::time_point now) const
{
    return std::chrono::duration<double>(now - m_requestTime).count();
}

void ConfigurationReadinessTrace::Summarize(const char* reason, Clock::time_point now)
{
    char text[400];
    std::snprintf(text, sizeof(text), "READINESS #%llu summary (%s, %.3f s): ready %s, first matching frame %s; %llu frames rendered, "
        "%llu in other configurations (%u distinct), %u extent changes",
        static_cast<unsigned long long>(m_requestNumber), reason, Since(now),
        m_readySeconds >= 0.0 ? (std::to_string(m_readySeconds) + " s").c_str() : "never",
        m_matchedSeconds >= 0.0 ? (std::to_string(m_matchedSeconds) + " s").c_str() : "never",
        static_cast<unsigned long long>(m_framesRendered), static_cast<unsigned long long>(m_framesNotMatching),
        m_renderedConfigurations, m_extentChanges);
    Log::Info(text);
}

void ConfigurationReadinessTrace::Observe(const ConfigurationReadiness& readiness, const RenderedConfiguration& rendered,
    bool frameRendered, Clock::time_point now)
{
    // A different request: close the previous one, start timing this one.
    if(!m_started || !(readiness.requirements == m_requirements))
    {
        if(m_started)
        {
            Summarize("replaced", now);
        }

        m_started = true;
        ++m_requestNumber;
        m_requirements = readiness.requirements;
        m_requestTime = now;
        m_dependencies = {};
        m_dependencies.fill(DependencyReadiness{ReadinessState::NotApplicable, "unobserved"});
        m_overall = ReadinessState::NotApplicable;
        m_readyLogged = false;
        m_readySeconds = -1.0;
        m_matchedLogged = false;
        m_matchedSeconds = -1.0;
        m_renderedKnown = false;
        m_framesRendered = 0;
        m_framesNotMatching = 0;
        m_renderedConfigurations = 0;
        m_extentChanges = 0;
        Log::Info("READINESS #" + std::to_string(m_requestNumber) + " requested: " + DescribeRequirements(m_requirements));
    }

    for(std::size_t index = 0; index < readiness.dependencies.size(); ++index)
    {
        const DependencyReadiness& dependency = readiness.dependencies[index];

        if(!(dependency == m_dependencies[index]))
        {
            char text[160];
            std::snprintf(text, sizeof(text), "READINESS #%llu +%.3f s %s: %s", static_cast<unsigned long long>(m_requestNumber), Since(now),
                ReadinessDependencyName(static_cast<ReadinessDependency>(index)), ReadinessStateName(dependency.state));
            Log::Info(std::string(text) + (dependency.detail.empty() ? "" : " (" + dependency.detail + ")"));
            m_dependencies[index] = dependency;
        }
    }

    if(readiness.overall != m_overall)
    {
        m_overall = readiness.overall;
        char text[128];
        std::snprintf(text, sizeof(text), "READINESS #%llu +%.3f s overall: %s", static_cast<unsigned long long>(m_requestNumber), Since(now),
            ReadinessStateName(m_overall));
        Log::Info(text);

        if(m_overall == ReadinessState::Ready && !m_readyLogged)
        {
            m_readyLogged = true;
            m_readySeconds = Since(now);
        }
    }

    ObserveRendered(rendered, frameRendered, now);
}

bool ConfigurationReadinessTrace::NeedsEvaluation(const RenderingConfiguration& configuration, unsigned int outputWidth,
    unsigned int outputHeight)
{
    const bool inputsChanged = !m_haveInputs || !(configuration == m_lastConfiguration) || outputWidth != m_lastOutputWidth ||
        outputHeight != m_lastOutputHeight;
    const bool settled = m_readyLogged && m_matchedLogged && m_overall == ReadinessState::Ready;

    if(inputsChanged || !settled || ++m_framesSinceEvaluation >= SettledEvaluationInterval)
    {
        m_haveInputs = true;
        m_lastConfiguration = configuration;
        m_lastOutputWidth = outputWidth;
        m_lastOutputHeight = outputHeight;
        m_framesSinceEvaluation = 0;
        return true;
    }

    return false;
}

void ConfigurationReadinessTrace::ObserveRendered(const RenderedConfiguration& rendered, bool frameRendered, Clock::time_point now)
{
    if(!m_started || !frameRendered)
    {
        return;
    }

    ++m_framesRendered;
    const bool matches = RenderedMatchesRequest(rendered, m_requirements);

    if(!matches)
    {
        ++m_framesNotMatching;
    }

    if(!m_renderedKnown || !(rendered == m_rendered))
    {
        if(m_renderedKnown && (rendered.renderWidth != m_rendered.renderWidth || rendered.renderHeight != m_rendered.renderHeight ||
            rendered.outputWidth != m_rendered.outputWidth || rendered.outputHeight != m_rendered.outputHeight))
        {
            ++m_extentChanges;
        }

        ++m_renderedConfigurations;
        m_rendered = rendered;
        m_renderedKnown = true;
        char text[96];
        std::snprintf(text, sizeof(text), "READINESS #%llu +%.3f s rendering%s: ", static_cast<unsigned long long>(m_requestNumber),
            Since(now), matches ? " (the request)" : " (NOT the request)");
        Log::Info(std::string(text) + DescribeRendered(rendered));
    }

    if(matches && !m_matchedLogged)
    {
        m_matchedLogged = true;
        m_matchedSeconds = Since(now);
        Summarize("first matching frame", now);
    }
}
