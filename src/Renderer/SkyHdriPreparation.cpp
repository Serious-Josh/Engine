#include "Renderer/SkyHdriPreparation.h"

#include <Windows.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace
{
constexpr double Pi = 3.14159265358979323846;
constexpr float HalfMaximum = 65504.0f;

std::uint16_t ToHalf(double value)
{
    const float clamped = std::isfinite(value) ? float((std::min)((std::max)(value, 0.0), double(HalfMaximum))) : 0.0f;
    return DirectX::PackedVector::XMConvertFloatToHalf(clamped);
}

// One source row resampled horizontally to `width` columns (RGB, doubles), the rotation applied. A narrower target integrates
// the piecewise-constant source over each target column's interval exactly (prefix sums, wrapping); a wider one interpolates.
class RowResampler
{
public:
    RowResampler(const HDRImageData& source, unsigned int width, double rotationTurns)
        : m_source(source), m_width(width), m_shift(rotationTurns - std::floor(rotationTurns)), m_prefix(3 * (source.width + 1))
    {
    }

    void Resample(unsigned int sourceRow, std::vector<double>& out)
    {
        const unsigned int sourceWidth = m_source.width;
        const float* row = m_source.pixels.data() + std::size_t(sourceRow) * sourceWidth * 4;
        out.assign(std::size_t(m_width) * 3, 0.0);

        if(m_width <= sourceWidth)
        {
            for(unsigned int x = 0; x < sourceWidth; ++x)
            {
                for(unsigned int c = 0; c < 3; ++c)
                {
                    const float value = row[std::size_t(x) * 4 + c];
                    m_prefix[3 * (x + 1) + c] = m_prefix[3 * x + c] + (std::isfinite(value) ? double(value) : 0.0);
                }
            }

            // Target column i covers source positions [(i / w - shift) W, ((i + 1) / w - shift) W).
            const double scale = double(sourceWidth) / double(m_width);

            for(unsigned int i = 0; i < m_width; ++i)
            {
                const double begin = (double(i) / m_width - m_shift) * sourceWidth;
                const double end = begin + scale;

                for(unsigned int c = 0; c < 3; ++c)
                {
                    out[std::size_t(i) * 3 + c] = (Integral(end, c) - Integral(begin, c)) / scale;
                }
            }

            return;
        }

        for(unsigned int i = 0; i < m_width; ++i)
        {
            double position = ((double(i) + 0.5) / m_width - m_shift) * sourceWidth - 0.5;
            position -= std::floor(position / sourceWidth) * sourceWidth;
            const unsigned int left = unsigned(position) % sourceWidth;
            const unsigned int right = (left + 1) % sourceWidth;
            const double t = position - std::floor(position);

            for(unsigned int c = 0; c < 3; ++c)
            {
                const double a = Finite(row[std::size_t(left) * 4 + c]);
                const double b = Finite(row[std::size_t(right) * 4 + c]);
                out[std::size_t(i) * 3 + c] = a + (b - a) * t;
            }
        }
    }

private:
    static double Finite(float value)
    {
        return std::isfinite(value) ? double(value) : 0.0;
    }

    // The integral of channel c from source position 0 to x (any real x: whole turns add the row total).
    double Integral(double x, unsigned int c) const
    {
        const unsigned int sourceWidth = m_source.width;
        const double turns = std::floor(x / sourceWidth);
        double inside = x - turns * sourceWidth;
        unsigned int texel = (std::min)(unsigned(inside), sourceWidth - 1);
        const double fraction = inside - texel;
        const double total = m_prefix[3 * sourceWidth + c];
        const double texelValue = m_prefix[3 * (texel + 1) + c] - m_prefix[3 * texel + c];
        return turns * total + m_prefix[3 * texel + c] + fraction * texelValue;
    }

    const HDRImageData& m_source;
    const unsigned int m_width;
    const double m_shift;
    std::vector<double> m_prefix;
};
}

std::string SkyHdriIdentity(const std::string& sourceIdentity, float radianceScale, float rotationDegrees)
{
    std::uint32_t scaleBits = 0;
    std::uint32_t rotationBits = 0;
    std::memcpy(&scaleBits, &radianceScale, sizeof(scaleBits));
    std::memcpy(&rotationBits, &rotationDegrees, sizeof(rotationBits));
    char suffix[48];
    std::snprintf(suffix, sizeof(suffix), "|scale=%08x|yaw=%08x", scaleBits, rotationBits);
    return sourceIdentity + suffix;
}

bool ResampleSkyHdri(const HDRImageData& source, float radianceScale, float rotationDegrees, unsigned int width,
    unsigned int height, std::vector<std::uint16_t>& texels, const std::function<bool()>& cancel)
{
    if(source.width == 0 || source.height == 0 || source.channels != 4 ||
        source.pixels.size() != std::size_t(source.width) * source.height * 4 || width == 0 || height == 0)
    {
        throw std::runtime_error("Invalid HDRI sky image dimensions or pixel storage.");
    }

    if(!std::isfinite(radianceScale) || radianceScale < 0.0f || !std::isfinite(rotationDegrees))
    {
        throw std::runtime_error("Invalid HDRI sky radiance scale or rotation.");
    }

    texels.assign(std::size_t(width) * height * 4, 0);
    RowResampler rows(source, width, double(rotationDegrees) / 360.0);
    std::vector<double> first;
    std::vector<double> second;
    std::vector<double> sum(std::size_t(width) * 3);
    const unsigned int sourceHeight = source.height;
    const std::uint16_t one = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);

    for(unsigned int y = 0; y < height; ++y)
    {
        if(cancel && cancel())
        {
            return false;
        }

        std::fill(sum.begin(), sum.end(), 0.0);

        if(height <= sourceHeight)
        {
            // Rows by polar angle from +Y: target row y covers [y pi / h, (y + 1) pi / h]; each source row it overlaps weighs by
            // the overlap's solid angle, cos(a) - cos(b).
            const double top = Pi * y / height;
            const double bottom = Pi * (y + 1) / height;
            const unsigned int firstRow = (std::min)(unsigned(std::floor(top / Pi * sourceHeight)), sourceHeight - 1);
            const unsigned int endRow = (std::min)(unsigned(std::ceil(bottom / Pi * sourceHeight)), sourceHeight);
            double weights = 0.0;

            for(unsigned int row = firstRow; row < endRow; ++row)
            {
                const double a = (std::max)(top, Pi * row / sourceHeight);
                const double b = (std::min)(bottom, Pi * (row + 1) / sourceHeight);

                if(!(b > a))
                {
                    continue;
                }

                const double weight = std::cos(a) - std::cos(b);
                rows.Resample(row, first);

                for(std::size_t i = 0; i < sum.size(); ++i)
                {
                    sum[i] += weight * first[i];
                }

                weights += weight;
            }

            const double normalization = weights > 0.0 ? 1.0 / weights : 0.0;

            for(double& value : sum)
            {
                value *= normalization;
            }
        }
        else
        {
            const double position = std::clamp((double(y) + 0.5) / height * sourceHeight - 0.5, 0.0, double(sourceHeight - 1));
            const unsigned int upper = unsigned(position);
            const unsigned int lower = (std::min)(upper + 1, sourceHeight - 1);
            const double t = position - upper;
            rows.Resample(upper, first);
            rows.Resample(lower, second);

            for(std::size_t i = 0; i < sum.size(); ++i)
            {
                sum[i] = first[i] + (second[i] - first[i]) * t;
            }
        }

        std::uint16_t* out = texels.data() + std::size_t(y) * width * 4;

        for(unsigned int x = 0; x < width; ++x)
        {
            for(unsigned int c = 0; c < 3; ++c)
            {
                out[std::size_t(x) * 4 + c] = ToHalf(sum[std::size_t(x) * 3 + c] * double(radianceScale));
            }

            out[std::size_t(x) * 4 + 3] = one;
        }
    }

    return true;
}

SkyHdriJob::SkyHdriJob(std::uint64_t requestTicket, SkyHdriSource requestSource, unsigned int width, unsigned int backgroundWidth)
    : ticket(requestTicket), source(std::move(requestSource)),
      requestedIdentity(source.identity.empty() ? std::string() :
          SkyHdriIdentity(source.identity, source.radianceScale, source.rotationDegrees)),
      requested(std::chrono::steady_clock::now()), m_width(width), m_backgroundWidth(backgroundWidth)
{
}

SkyHdriJob::~SkyHdriJob()
{
    Cancel();

    if(m_worker.joinable())
    {
        m_worker.join();
    }
}

void SkyHdriJob::Start()
{
    m_worker = std::thread([this]() { Run(); });
}

std::shared_ptr<const SkyHdriTexels> SkyHdriJob::TakeResult(std::string& error)
{
    if(m_worker.joinable())
    {
        m_worker.join();
    }

    error = m_error;
    return m_result;
}

void SkyHdriJob::Run()
{
    // Background work must not compete with the frame thread.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto cancelled = [this]() { return m_cancelled.load(std::memory_order_relaxed); };

    try
    {
        if(!cancelled())
        {
            std::string sourceIdentity = source.identity;
            HDRImageData image = source.load(sourceIdentity);
            auto result = std::make_shared<SkyHdriTexels>();
            result->identity = SkyHdriIdentity(sourceIdentity, source.radianceScale, source.rotationDegrees);
            result->displayName = source.displayName;
            result->width = m_width;
            result->height = m_width / 2;
            bool complete = ResampleSkyHdri(image, source.radianceScale, source.rotationDegrees, result->width, result->height,
                result->texels, cancelled);

            if(complete && m_backgroundWidth > 0)
            {
                result->backgroundWidth = m_backgroundWidth;
                result->backgroundHeight = m_backgroundWidth / 2;
                complete = ResampleSkyHdri(image, source.radianceScale, source.rotationDegrees, result->backgroundWidth,
                    result->backgroundHeight, result->background, cancelled);
            }

            if(complete)
            {
                m_result = std::move(result);
            }
        }
    }
    catch(const std::exception& exception)
    {
        m_result.reset();
        m_error = exception.what();
    }
    catch(...)
    {
        m_result.reset();
        m_error = "HDRI sky preparation failed.";
    }

    if(!m_result && m_error.empty() && !cancelled())
    {
        m_error = "HDRI sky preparation failed.";
    }

    m_done.store(true, std::memory_order_release);
}
