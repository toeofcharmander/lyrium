#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>

#include <d3d9.h>

#include "eluvian/diag/texture_size.h"

namespace eluvian
{

class TextureStager
{
  public:
    static auto instance() -> TextureStager &
    {
        static auto stager = TextureStager{};
        return stager;
    }

    auto configure(bool enabled, ::IDirect3DDevice9 *device) -> void
    {
        device_.store(device, std::memory_order_relaxed);
        auto lock = std::scoped_lock{mutex_};
        enabled_ = enabled;
    }

    auto enabled() const -> bool
    {
        return enabled_;
    }

    struct Shape
    {
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t levels;
        ::D3DFORMAT format;
    };

    auto track(::IDirect3DTexture9 *texture, const Shape &shape) -> void
    {
        auto lock = std::scoped_lock{mutex_};
        staged_[texture] =
            Entry{.shape = shape, .staging = nullptr, .locked_level = 0, .has_rect = false, .rect = {}};
    }

    auto forget(::IDirect3DTexture9 *texture) -> void
    {
        ::IDirect3DTexture9 *staging = nullptr;
        {
            auto lock = std::scoped_lock{mutex_};
            const auto found = staged_.find(texture);
            if (found == staged_.end())
            {
                return;
            }
            staging = found->second.staging;
            staged_.erase(found);
        }
        if (staging != nullptr)
        {
            staging->Release();
        }
    }

    auto is_staged(::IDirect3DTexture9 *texture) -> bool
    {
        auto lock = std::scoped_lock{mutex_};
        return staged_.contains(texture);
    }

    auto lock_rect(
        ::IDirect3DTexture9 *texture,
        ::UINT level,
        ::D3DLOCKED_RECT *locked,
        const ::RECT *rect,
        ::DWORD flags) -> ::HRESULT
    {

        auto *device = device_.load(std::memory_order_relaxed);
        if (device == nullptr)
        {
            return E_FAIL;
        }

        auto shape = Shape{};
        auto *staging = static_cast<::IDirect3DTexture9 *>(nullptr);
        {
            auto lock = std::scoped_lock{mutex_};
            const auto found = staged_.find(texture);
            if (found == staged_.end())
            {
                return E_FAIL;
            }
            shape = found->second.shape;
            staging = found->second.staging;
            found->second.locked_level = level;
            found->second.has_rect = rect != nullptr;
            if (rect != nullptr)
            {
                found->second.rect = *rect;
            }
        }

        if (staging == nullptr)
        {
            auto *created = static_cast<::IDirect3DTexture9 *>(nullptr);
            const auto hr = device->CreateTexture(
                shape.width, shape.height, shape.levels, 0, shape.format,
                D3DPOOL_SYSTEMMEM, &created, nullptr);
            if (FAILED(hr) || created == nullptr)
            {
                auto lock = std::scoped_lock{mutex_};
                ++failures_;
                return FAILED(hr) ? hr : E_FAIL;
            }

            auto duplicate = static_cast<::IDirect3DTexture9 *>(nullptr);
            {
                auto lock = std::scoped_lock{mutex_};
                const auto found = staged_.find(texture);
                if (found == staged_.end())
                {
                    duplicate = created;
                }
                else if (found->second.staging != nullptr)
                {
                    duplicate = created;
                    staging = found->second.staging;
                }
                else
                {
                    found->second.staging = created;
                    staging = created;
                    ++staging_created_;
                }
            }

            if (duplicate != nullptr)
            {
                duplicate->Release();
            }
            if (staging == nullptr)
            {
                return E_FAIL;
            }
        }

        return staging->LockRect(level, locked, rect, flags);
    }

    auto unlock_rect(::IDirect3DTexture9 *texture, ::UINT level) -> ::HRESULT
    {
        ::IDirect3DTexture9 *staging = nullptr;
        auto shape = Shape{};
        auto has_rect = false;
        auto rect = ::RECT{};
        {
            auto lock = std::scoped_lock{mutex_};
            const auto found = staged_.find(texture);
            if (found == staged_.end() || found->second.staging == nullptr)
            {
                return E_FAIL;
            }
            staging = found->second.staging;
            shape = found->second.shape;
            has_rect = found->second.has_rect;
            rect = found->second.rect;
        }

        auto hr = staging->UnlockRect(level);
        if (FAILED(hr))
        {
            return hr;
        }

        auto *device = device_.load(std::memory_order_relaxed);
        ::IDirect3DSurface9 *source = nullptr;
        ::IDirect3DSurface9 *destination = nullptr;
        auto succeeded = false;

        if (device != nullptr && SUCCEEDED(staging->GetSurfaceLevel(level, &source)) &&
            SUCCEEDED(texture->GetSurfaceLevel(level, &destination)))
        {

            const auto compressed = diag::is_block_compressed(shape.format);
            const auto level_width = std::max<::LONG>(static_cast<::LONG>(shape.width >> level), 1);
            const auto level_height = std::max<::LONG>(static_cast<::LONG>(shape.height >> level), 1);

            if (has_rect)
            {
                auto source_rect = rect;
                if (compressed)
                {
                    source_rect.left &= ~3;
                    source_rect.top &= ~3;
                    source_rect.right = (source_rect.right + 3) & ~3;
                    source_rect.bottom = (source_rect.bottom + 3) & ~3;
                }

                source_rect.left = std::max<::LONG>(source_rect.left, 0);
                source_rect.top = std::max<::LONG>(source_rect.top, 0);
                source_rect.right = std::min<::LONG>(source_rect.right, level_width);
                source_rect.bottom = std::min<::LONG>(source_rect.bottom, level_height);

                const auto covers_level = source_rect.left == 0 && source_rect.top == 0 &&
                                          source_rect.right >= level_width &&
                                          source_rect.bottom >= level_height;

                if (source_rect.right <= source_rect.left || source_rect.bottom <= source_rect.top)
                {
                    hr = D3D_OK;
                    succeeded = true;
                }
                else if (covers_level)
                {

                    hr = device->UpdateSurface(source, nullptr, destination, nullptr);
                    succeeded = SUCCEEDED(hr);
                }
                else
                {
                    const auto destination_point =
                        ::POINT{.x = source_rect.left, .y = source_rect.top};
                    hr = device->UpdateSurface(source, &source_rect, destination, &destination_point);
                    succeeded = SUCCEEDED(hr);
                }
            }
            else
            {
                hr = device->UpdateSurface(source, nullptr, destination, nullptr);
                succeeded = SUCCEEDED(hr);
            }
        }
        if (source != nullptr)
        {
            source->Release();
        }
        if (destination != nullptr)
        {
            destination->Release();
        }

        {
            auto lock = std::scoped_lock{mutex_};
            if (succeeded)
            {
                ++uploads_;
            }
            else
            {
                ++failures_;

                if (first_failure_hr_ == 0)
                {
                    first_failure_hr_ = static_cast<std::int32_t>(hr);
                    first_failure_format_ = static_cast<std::uint32_t>(shape.format);
                    first_failure_width_ = shape.width;
                    first_failure_height_ = shape.height;
                }
                failed_formats_[static_cast<std::uint32_t>(shape.format)]++;
            }
        }

        if (level + 1u >= shape.levels || shape.levels == 0u)
        {
            auto *finished = static_cast<::IDirect3DTexture9 *>(nullptr);
            {
                auto lock = std::scoped_lock{mutex_};
                const auto found = staged_.find(texture);
                if (found != staged_.end() && found->second.staging != nullptr)
                {
                    finished = found->second.staging;
                    found->second.staging = nullptr;
                    ++staging_released_;
                }
            }
            if (finished != nullptr)
            {
                finished->Release();
            }
        }

        return hr;
    }

    struct Stats
    {
        std::uint64_t tracked;
        std::uint64_t staging_created;
        std::uint64_t staging_released;
        std::uint64_t uploads;
        std::uint64_t failures;
        std::int32_t first_failure_hr;
        std::uint32_t first_failure_format;
        std::uint32_t first_failure_width;
        std::uint32_t first_failure_height;
        std::uint32_t worst_format;
        std::uint64_t worst_format_failures;
    };

    auto stats() -> Stats
    {
        auto lock = std::scoped_lock{mutex_};

        auto worst = std::uint32_t{0};
        auto worst_count = std::uint64_t{0};
        for (const auto &[format, count] : failed_formats_)
        {
            if (count > worst_count)
            {
                worst = format;
                worst_count = count;
            }
        }

        return Stats{
            .tracked = staged_.size(),
            .staging_created = staging_created_,
            .staging_released = staging_released_,
            .uploads = uploads_,
            .failures = failures_,
            .first_failure_hr = first_failure_hr_,
            .first_failure_format = first_failure_format_,
            .first_failure_width = first_failure_width_,
            .first_failure_height = first_failure_height_,
            .worst_format = worst,
            .worst_format_failures = worst_count,
        };
    }

  private:
    struct Entry
    {
        Shape shape;
        ::IDirect3DTexture9 *staging;
        ::UINT locked_level;

        bool has_rect;
        ::RECT rect;
    };

    TextureStager() = default;

    std::mutex mutex_;
    std::map<::IDirect3DTexture9 *, Entry> staged_;

    std::atomic<::IDirect3DDevice9 *> device_{nullptr};
    std::uint64_t staging_created_{0};
    std::uint64_t staging_released_{0};
    std::uint64_t uploads_{0};
    std::uint64_t failures_{0};
    std::int32_t first_failure_hr_{0};
    std::uint32_t first_failure_format_{0};
    std::uint32_t first_failure_width_{0};
    std::uint32_t first_failure_height_{0};
    std::map<std::uint32_t, std::uint64_t> failed_formats_;
    bool enabled_{false};
};

}
