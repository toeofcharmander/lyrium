#include "lyrium/resettable_texture.h"
#include "lyrium/com/ref_count.h"
#include "lyrium/texture/dirty_levels.h"
#include "lyrium/texture/level_validity.h"
#include "lyrium/texture/mip_layout.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

#include <windows.h>

#include "lyrium/allocators/global_allocator.h"
#include "lyrium/containers/map.h"
#include "lyrium/containers/unordered_map.h"
#include "lyrium/containers/unordered_set.h"
#include "lyrium/containers/vector.h"
#include "lyrium/diag/texture_size.h"
#include "lyrium/log.h"
#include "lyrium/stats.h"
#include "lyrium/texture/staging_pool.h"

namespace lyrium
{

namespace
{

// One pool for the process: staging textures are children of the single device
// the game creates. SYSTEMMEM resources survive device Reset, so the pool needs
// no reset handling, and it is never destroyed for the same reason nothing
// static here is. The budget is the pool's permanent address-space cost; see
// staging_pool.h for why paying it is the right trade.
auto staging_pool() -> texture::BasicStagingPool<::IDirect3DTexture9 *, STDAllocator> &
{
    static auto pool = texture::BasicStagingPool<::IDirect3DTexture9 *, STDAllocator>{24ull * 1024ull * 1024ull, 64u};
    return pool;
}

std::mutex staging_pool_mutex{};

auto acquire_staging(
    ::IDirect3DDevice9 *device,
    CreateTextureFn create_texture,
    std::uint32_t width,
    std::uint32_t height,
    ::D3DFORMAT format) -> ::IDirect3DTexture9 *
{
    const auto shape =
        texture::StagingShape{.width = width, .height = height, .format = static_cast<std::uint32_t>(format)};
    {
        auto lock = std::scoped_lock{staging_pool_mutex};
        if (const auto pooled = staging_pool().acquire(shape); pooled.has_value())
        {
            stats::staging_reused.fetch_add(1u, std::memory_order_relaxed);
            return *pooled;
        }
    }

    auto *staging = static_cast<::IDirect3DTexture9 *>(nullptr);
    const auto result = create_texture(device, width, height, 1u, 0u, format, D3DPOOL_SYSTEMMEM, &staging, nullptr);
    if (FAILED(result) || staging == nullptr)
    {
        return nullptr;
    }
    stats::staging_created.fetch_add(1u, std::memory_order_relaxed);
    return staging;
}

auto release_staging(
    ::IDirect3DTexture9 *staging,
    std::uint32_t width,
    std::uint32_t height,
    ::D3DFORMAT format,
    std::uint64_t bytes) -> void
{
    const auto shape =
        texture::StagingShape{.width = width, .height = height, .format = static_cast<std::uint32_t>(format)};
    {
        auto lock = std::scoped_lock{staging_pool_mutex};
        if (staging_pool().offer(shape, bytes, staging))
        {
            return;
        }
    }
    staging->Release();
}

class ResettableTexture;

std::mutex registry_mutex;
UnorderedSet<ResettableTexture *> registered_textures;
UnorderedMap<::IDirect3DBaseTexture9 *, ResettableTexture *> textures_by_inner;

class ResettableSurface final : public ::IDirect3DSurface9
{
  public:
    ResettableSurface(ResettableTexture *texture, ::UINT level);

    auto STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) -> ::HRESULT override;
    auto STDMETHODCALLTYPE AddRef() -> ::ULONG override;
    auto STDMETHODCALLTYPE Release() -> ::ULONG override;
    auto STDMETHODCALLTYPE GetDevice(::IDirect3DDevice9 **device) -> ::HRESULT override;
    auto STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, ::DWORD size, ::DWORD flags)
        -> ::HRESULT override;
    auto STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, ::DWORD *size) -> ::HRESULT override;
    auto STDMETHODCALLTYPE FreePrivateData(REFGUID guid) -> ::HRESULT override;
    auto STDMETHODCALLTYPE SetPriority(::DWORD priority) -> ::DWORD override;
    auto STDMETHODCALLTYPE GetPriority() -> ::DWORD override;
    auto STDMETHODCALLTYPE PreLoad() -> void override;
    auto STDMETHODCALLTYPE GetType() -> ::D3DRESOURCETYPE override;
    auto STDMETHODCALLTYPE GetContainer(REFIID iid, void **container) -> ::HRESULT override;
    auto STDMETHODCALLTYPE GetDesc(::D3DSURFACE_DESC *desc) -> ::HRESULT override;
    auto STDMETHODCALLTYPE LockRect(::D3DLOCKED_RECT *locked, const ::RECT *rect, ::DWORD flags) -> ::HRESULT override;
    auto STDMETHODCALLTYPE UnlockRect() -> ::HRESULT override;
    auto STDMETHODCALLTYPE GetDC(::HDC *dc) -> ::HRESULT override;
    auto STDMETHODCALLTYPE ReleaseDC(::HDC dc) -> ::HRESULT override;

    static auto operator new(std::size_t size) -> void *
    {
        return GlobalAllocator::allocate(size);
    }

    static auto operator delete(void *allocation) noexcept -> void
    {
        GlobalAllocator::deallocate(allocation);
    }

  private:
    ~ResettableSurface();

    auto surface() -> ::IDirect3DSurface9 *;

    std::atomic<::ULONG> references_{1u};
    ResettableTexture *texture_;
    ::UINT level_;
};

class ResettableTexture final : public ::IDirect3DTexture9
{
  public:
    static auto create(
        ::IDirect3DDevice9 *device,
        CreateTextureFn create_texture,
        ::IDirect3DTexture9 *texture,
        const ResettableTextureShape &shape,
        TextureDestroyedFn destroyed) -> ResettableTexture *
    {
        try
        {
            auto *result = new ResettableTexture{device, create_texture, texture, shape, destroyed};
            if (result->mapping_ == nullptr)
            {
                delete result;
                return nullptr;
            }
            {
                auto lock = std::scoped_lock{registry_mutex};
                registered_textures.insert(result);
                textures_by_inner[texture] = result;
            }
            return result;
        }
        catch (...)
        {
            return nullptr;
        }
    }

    auto STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) -> ::HRESULT override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        *object = nullptr;
        if (::IsEqualIID(iid, __uuidof(IUnknown)) || ::IsEqualIID(iid, __uuidof(IDirect3DResource9)) ||
            ::IsEqualIID(iid, __uuidof(IDirect3DBaseTexture9)) || ::IsEqualIID(iid, __uuidof(IDirect3DTexture9)))
        {
            *object = static_cast<::IDirect3DTexture9 *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    auto STDMETHODCALLTYPE AddRef() -> ::ULONG override
    {
        return references_.add_ref();
    }

    auto STDMETHODCALLTYPE Release() -> ::ULONG override
    {
        const auto remaining = references_.release();
        if (remaining == 0u)
        {
            delete this;
        }
        return remaining;
    }

    // Takes a reference only if the object is not already dying. The registry
    // walk uses this instead of AddRef, because an entry is still present in the
    // registry between Release observing zero and the destructor removing it.
    [[nodiscard]] auto try_add_ref() -> bool
    {
        return references_.try_add_ref();
    }

    auto STDMETHODCALLTYPE GetDevice(::IDirect3DDevice9 **device) -> ::HRESULT override
    {
        if (device == nullptr)
        {
            return D3DERR_INVALIDCALL;
        }
        *device = device_;
        device_->AddRef();
        return D3D_OK;
    }

    auto STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, ::DWORD size, ::DWORD flags)
        -> ::HRESULT override
    {
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return D3DERR_INVALIDCALL;
        }
        const auto result = texture->SetPrivateData(guid, data, size, flags);
        texture->Release();
        return result;
    }

    auto STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, ::DWORD *size) -> ::HRESULT override
    {
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return D3DERR_INVALIDCALL;
        }
        const auto result = texture->GetPrivateData(guid, data, size);
        texture->Release();
        return result;
    }

    auto STDMETHODCALLTYPE FreePrivateData(REFGUID guid) -> ::HRESULT override
    {
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return D3DERR_INVALIDCALL;
        }
        const auto result = texture->FreePrivateData(guid);
        texture->Release();
        return result;
    }

    auto STDMETHODCALLTYPE SetPriority(::DWORD priority) -> ::DWORD override
    {
        const auto previous = priority_.exchange(priority, std::memory_order_relaxed);
        auto *texture = acquire_inner();
        if (texture != nullptr)
        {
            texture->SetPriority(priority);
            texture->Release();
        }
        return previous;
    }

    auto STDMETHODCALLTYPE GetPriority() -> ::DWORD override
    {
        return priority_.load(std::memory_order_relaxed);
    }

    auto STDMETHODCALLTYPE PreLoad() -> void override
    {
        auto *texture = acquire_inner();
        if (texture != nullptr)
        {
            texture->PreLoad();
            texture->Release();
        }
    }

    auto STDMETHODCALLTYPE GetType() -> ::D3DRESOURCETYPE override
    {
        return D3DRTYPE_TEXTURE;
    }

    auto STDMETHODCALLTYPE SetLOD(::DWORD lod) -> ::DWORD override
    {
        return lod_.exchange(lod, std::memory_order_relaxed);
    }

    auto STDMETHODCALLTYPE GetLOD() -> ::DWORD override
    {
        return lod_.load(std::memory_order_relaxed);
    }

    auto STDMETHODCALLTYPE GetLevelCount() -> ::DWORD override
    {
        return shape_.levels;
    }

    auto STDMETHODCALLTYPE SetAutoGenFilterType(::D3DTEXTUREFILTERTYPE type) -> ::HRESULT override
    {
        filter_.store(type, std::memory_order_relaxed);
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return D3DERR_INVALIDCALL;
        }
        const auto result = texture->SetAutoGenFilterType(type);
        texture->Release();
        return result;
    }

    auto STDMETHODCALLTYPE GetAutoGenFilterType() -> ::D3DTEXTUREFILTERTYPE override
    {
        return filter_.load(std::memory_order_relaxed);
    }

    auto STDMETHODCALLTYPE GenerateMipSubLevels() -> void override
    {
        auto *texture = acquire_inner();
        if (texture != nullptr)
        {
            texture->GenerateMipSubLevels();
            texture->Release();
        }
    }

    auto STDMETHODCALLTYPE GetLevelDesc(::UINT level, ::D3DSURFACE_DESC *desc) -> ::HRESULT override
    {
        if (desc == nullptr || level >= layouts_.size())
        {
            return D3DERR_INVALIDCALL;
        }
        const auto &layout = layouts_[level];
        *desc = ::D3DSURFACE_DESC{
            .Format = shape_.format,
            .Type = D3DRTYPE_SURFACE,
            .Usage = shape_.usage,
            .Pool = D3DPOOL_MANAGED,
            .MultiSampleType = D3DMULTISAMPLE_NONE,
            .MultiSampleQuality = 0u,
            .Width = layout.width,
            .Height = layout.height,
        };
        return D3D_OK;
    }

    auto STDMETHODCALLTYPE GetSurfaceLevel(::UINT level, ::IDirect3DSurface9 **surface) -> ::HRESULT override
    {
        if (surface == nullptr || level >= layouts_.size())
        {
            return D3DERR_INVALIDCALL;
        }
        try
        {
            *surface = new ResettableSurface{this, level};
            return D3D_OK;
        }
        catch (...)
        {
            *surface = nullptr;
            return E_OUTOFMEMORY;
        }
    }

    auto STDMETHODCALLTYPE LockRect(::UINT level, ::D3DLOCKED_RECT *locked, const ::RECT *rect, ::DWORD flags)
        -> ::HRESULT override
    {
        if (locked == nullptr || level >= layouts_.size())
        {
            return D3DERR_INVALIDCALL;
        }

        const auto &layout = layouts_[level];
        if (rect != nullptr && (rect->left < 0 || rect->top < 0 || rect->right <= rect->left ||
                                rect->bottom <= rect->top || static_cast<std::uint32_t>(rect->right) > layout.width ||
                                static_cast<std::uint32_t>(rect->bottom) > layout.height))
        {
            return D3DERR_INVALIDCALL;
        }

        auto lock = std::scoped_lock{lock_mutex_};
        auto &state = locks_[level];
        if (state.view != nullptr)
        {
            return D3DERR_INVALIDCALL;
        }

        const auto map_started = now_us();
        auto *view = static_cast<std::byte *>(
            ::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0u, 0u, static_cast<::SIZE_T>(mapping_size_)));
        stats::locks.fetch_add(1u, std::memory_order_relaxed);
        stats::map_us.fetch_add(static_cast<std::uint64_t>(now_us() - map_started), std::memory_order_relaxed);
        if (view == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        auto x = std::uint32_t{};
        auto y = std::uint32_t{};
        if (rect != nullptr)
        {
            x = static_cast<std::uint32_t>(rect->left) / 4u;
            y = static_cast<std::uint32_t>(rect->top) / 4u;
        }

        state.view = view;
        state.flags = flags;
        locked->Pitch = static_cast<::INT>(layout.pitch);
        locked->pBits = view + layout.offset + static_cast<std::size_t>(y) * layout.pitch +
                        static_cast<std::size_t>(x) * block_bytes_;
        return D3D_OK;
    }

    auto STDMETHODCALLTYPE UnlockRect(::UINT level) -> ::HRESULT override
    {
        if (level >= layouts_.size())
        {
            return D3DERR_INVALIDCALL;
        }

        void *view = nullptr;
        auto flags = ::DWORD{};
        {
            auto lock = std::scoped_lock{lock_mutex_};
            auto &state = locks_[level];
            if (state.view == nullptr)
            {
                return D3DERR_INVALIDCALL;
            }
            view = state.view;
            flags = state.flags;
            state.view = nullptr;
            state.flags = 0u;
        }

        auto result = D3D_OK;
        if ((flags & D3DLOCK_READONLY) == 0u)
        {
            // Owed, not uploaded. The write is already in the mapped section,
            // which is the durable copy; flush_dirty() hands it to the GPU in one
            // batch when the engine binds the texture. See dirty_levels.h.
            valid_.mark_valid(level);
            dirty_.mark(level);
        }
        const auto unmap_started = now_us();
        ::UnmapViewOfFile(view);
        stats::unmap_us.fetch_add(static_cast<std::uint64_t>(now_us() - unmap_started), std::memory_order_relaxed);
        return result;
    }

    auto STDMETHODCALLTYPE AddDirtyRect(const ::RECT *) -> ::HRESULT override
    {
        return D3D_OK;
    }

    auto acquire_inner() -> ::IDirect3DTexture9 *
    {
        if (!ensure_inner())
        {
            return nullptr;
        }
        auto lock = std::scoped_lock{inner_mutex_};
        if (inner_ != nullptr)
        {
            inner_->AddRef();
        }
        return inner_;
    }

    // Takes a reference while holding the lock, so the returned pointer cannot
    // be released by a concurrent reset before the caller has used it.
    auto acquire_inner_for_binding() -> ::IDirect3DBaseTexture9 *
    {
        if (!ensure_inner())
        {
            return nullptr;
        }

        // The one point where the GPU copy has to be current: the engine is
        // about to draw with it. Everything written since the last bind goes up
        // in a single batch here rather than a call per level at unlock time.
        flush_dirty();

        auto lock = std::scoped_lock{inner_mutex_};
        if (inner_ != nullptr)
        {
            inner_->AddRef();
        }
        return inner_;
    }

    auto surface_for_level(::UINT level) -> ::IDirect3DSurface9 *
    {
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return nullptr;
        }
        auto *surface = static_cast<::IDirect3DSurface9 *>(nullptr);
        texture->GetSurfaceLevel(level, &surface);
        texture->Release();
        return surface;
    }

    auto release_inner() -> void
    {
        auto *released = static_cast<::IDirect3DTexture9 *>(nullptr);
        {
            auto lock = std::scoped_lock{inner_mutex_};
            released = inner_;
            inner_ = nullptr;
        }
        if (released != nullptr)
        {
            {
                auto lock = std::scoped_lock{registry_mutex};
                textures_by_inner.erase(released);
            }
            released->Release();
        }
    }

    auto restore_inner() -> void
    {
        ensure_inner();
    }

    static auto operator new(std::size_t size) -> void *
    {
        return GlobalAllocator::allocate(size);
    }

    static auto operator delete(void *allocation) noexcept -> void
    {
        GlobalAllocator::deallocate(allocation);
    }

  private:
    using Layout = texture::MipLevel;

    struct LockState
    {
        void *view;
        ::DWORD flags;
    };

    ResettableTexture(
        ::IDirect3DDevice9 *device,
        CreateTextureFn create_texture,
        ::IDirect3DTexture9 *texture,
        const ResettableTextureShape &shape,
        TextureDestroyedFn destroyed)
        : device_{device}
        , create_texture_{create_texture}
        , inner_{texture}
        , shape_{shape}
        , destroyed_{destroyed}
    {
        device_->AddRef();
        texture->AddRef();
        shape_.levels = texture->GetLevelCount();
        block_bytes_ = diag::format_bits_per_pixel(shape_.format) == 4u ? 8u : 16u;

        locks_.resize(shape_.levels, LockState{.view = nullptr, .flags = 0u});

        // The arithmetic lives in texture/mip_layout.h, where it is tested. It
        // decides the length of the mapping below and every offset handed back
        // from LockRect, so it is worth having covered rather than inlined here.
        mapping_size_ = texture::append_mip_levels(layouts_, shape_.width, shape_.height, shape_.levels, block_bytes_);
        if (mapping_size_ == 0u || mapping_size_ > std::numeric_limits<::SIZE_T>::max())
        {
            return;
        }
        const auto size = static_cast<std::uint64_t>(mapping_size_);
        const auto mapping_started = now_us();
        mapping_ = ::CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            static_cast<::DWORD>(size >> 32u),
            static_cast<::DWORD>(size & 0xffffffffu),
            nullptr);
        stats::mapping_creates.fetch_add(1u, std::memory_order_relaxed);
        stats::mapping_create_us.fetch_add(
            static_cast<std::uint64_t>(now_us() - mapping_started), std::memory_order_relaxed);
    }

    ~ResettableTexture()
    {
        {
            auto lock = std::scoped_lock{registry_mutex};
            registered_textures.erase(this);
            if (inner_ != nullptr)
            {
                textures_by_inner.erase(inner_);
            }
        }

        for (auto &state : locks_)
        {
            if (state.view != nullptr)
            {
                ::UnmapViewOfFile(state.view);
            }
        }
        if (inner_ != nullptr)
        {
            inner_->Release();
        }
        if (mapping_ != nullptr)
        {
            ::CloseHandle(mapping_);
        }
        device_->Release();
        if (destroyed_ != nullptr)
        {
            destroyed_(this);
        }
    }

    auto ensure_inner() -> bool
    {
        {
            auto lock = std::scoped_lock{inner_mutex_};
            if (inner_ != nullptr)
            {
                return true;
            }
        }

        auto *created = static_cast<::IDirect3DTexture9 *>(nullptr);
        const auto result = create_texture_(
            device_,
            shape_.width,
            shape_.height,
            shape_.levels,
            shape_.usage,
            shape_.format,
            D3DPOOL_DEFAULT,
            &created,
            nullptr);
        if (FAILED(result) || created == nullptr)
        {
            return false;
        }

        {
            auto lock = std::scoped_lock{inner_mutex_};
            if (inner_ != nullptr)
            {
                created->Release();
                return true;
            }
            inner_ = created;
        }
        {
            auto lock = std::scoped_lock{registry_mutex};
            textures_by_inner[created] = this;
        }

        created->SetPriority(priority_.load(std::memory_order_relaxed));
        created->SetAutoGenFilterType(filter_.load(std::memory_order_relaxed));

        // A reset discards the GPU-side copy of every level, so everything
        // holding data is owed again. Deliberately not uploaded here: a reset
        // restores hundreds of textures at once and most are not drawn in the
        // next frame, which is exactly the burst that made alt-tab expensive.
        dirty_.mark_all(valid_.mask());
        return true;
    }

    auto upload_level(::UINT level, const std::byte *view) -> ::HRESULT
    {
        auto *texture = acquire_inner();
        if (texture == nullptr)
        {
            return D3DERR_DEVICELOST;
        }

        const auto started = now_us();
        const auto &layout = layouts_[level];
        const auto staging_bytes = static_cast<std::uint64_t>(layout.pitch) * layout.rows;
        auto *staging = acquire_staging(device_, create_texture_, layout.width, layout.height, shape_.format);
        auto result = staging != nullptr ? D3D_OK : E_OUTOFMEMORY;
        if (staging != nullptr)
        {
            auto locked = ::D3DLOCKED_RECT{};
            result = staging->LockRect(0u, &locked, nullptr, 0u);
            if (SUCCEEDED(result))
            {
                const auto *source = view + layout.offset;
                auto *destination = static_cast<std::byte *>(locked.pBits);
                for (auto row = std::uint32_t{}; row < layout.rows; ++row)
                {
                    std::memcpy(
                        destination + static_cast<std::size_t>(row) * static_cast<std::size_t>(locked.Pitch),
                        source + static_cast<std::size_t>(row) * layout.pitch,
                        layout.pitch);
                }
                result = staging->UnlockRect(0u);
            }

            auto *source_surface = static_cast<::IDirect3DSurface9 *>(nullptr);
            auto *destination_surface = static_cast<::IDirect3DSurface9 *>(nullptr);
            if (SUCCEEDED(result))
            {
                result = staging->GetSurfaceLevel(0u, &source_surface);
            }
            if (SUCCEEDED(result))
            {
                result = texture->GetSurfaceLevel(level, &destination_surface);
            }
            if (SUCCEEDED(result))
            {
                result = device_->UpdateSurface(source_surface, nullptr, destination_surface, nullptr);
            }
            if (source_surface != nullptr)
            {
                source_surface->Release();
            }
            if (destination_surface != nullptr)
            {
                destination_surface->Release();
            }
            release_staging(staging, layout.width, layout.height, shape_.format, staging_bytes);
        }
        texture->Release();
        stats::uploads.fetch_add(1u, std::memory_order_relaxed);
        stats::upload_us.fetch_add(static_cast<std::uint64_t>(now_us() - started), std::memory_order_relaxed);
        return result;
    }

    // Settles every level owed, mapping the section once for the whole batch
    // rather than once per level.
    auto flush_dirty() -> void
    {
        if (!dirty_.any())
        {
            return;
        }

        const auto owed = dirty_.take();
        if (owed == 0u)
        {
            return;
        }

        auto *view = static_cast<const std::byte *>(
            ::MapViewOfFile(mapping_, FILE_MAP_READ, 0u, 0u, static_cast<::SIZE_T>(mapping_size_)));
        if (view == nullptr)
        {
            // Nothing was uploaded, so the debt is still owed.
            dirty_.mark_all(owed);
            return;
        }

        for (auto level = std::uint32_t{}; level < shape_.levels; ++level)
        {
            if ((owed & (std::uint32_t{1} << level)) != 0u)
            {
                upload_level(level, view);
            }
        }
        ::UnmapViewOfFile(view);
        stats::flushes.fetch_add(1u, std::memory_order_relaxed);
    }

    texture::DirtyLevels dirty_{};
    com::RefCount references_{1u};
    ::IDirect3DDevice9 *device_;
    CreateTextureFn create_texture_;
    std::mutex inner_mutex_;
    ::IDirect3DTexture9 *inner_;
    ResettableTextureShape shape_;
    TextureDestroyedFn destroyed_;
    ::HANDLE mapping_{nullptr};
    std::size_t mapping_size_{0u};
    std::uint32_t block_bytes_{0u};
    Vector<Layout> layouts_;
    Vector<LockState> locks_;
    texture::LevelValidity valid_;
    std::mutex lock_mutex_;
    std::atomic<::DWORD> priority_{0u};
    std::atomic<::DWORD> lod_{0u};
    std::atomic<::D3DTEXTUREFILTERTYPE> filter_{D3DTEXF_POINT};
};

ResettableSurface::ResettableSurface(ResettableTexture *texture, ::UINT level)
    : texture_{texture}
    , level_{level}
{
    texture_->AddRef();
}

ResettableSurface::~ResettableSurface()
{
    texture_->Release();
}

auto STDMETHODCALLTYPE ResettableSurface::QueryInterface(REFIID iid, void **object) -> ::HRESULT
{
    if (object == nullptr)
    {
        return E_POINTER;
    }
    *object = nullptr;
    if (::IsEqualIID(iid, __uuidof(IUnknown)) || ::IsEqualIID(iid, __uuidof(IDirect3DResource9)) ||
        ::IsEqualIID(iid, __uuidof(IDirect3DSurface9)))
    {
        *object = static_cast<::IDirect3DSurface9 *>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

auto STDMETHODCALLTYPE ResettableSurface::AddRef() -> ::ULONG
{
    return references_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

auto STDMETHODCALLTYPE ResettableSurface::Release() -> ::ULONG
{
    const auto remaining = references_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (remaining == 0u)
    {
        delete this;
    }
    return remaining;
}

auto ResettableSurface::surface() -> ::IDirect3DSurface9 *
{
    return texture_->surface_for_level(level_);
}

auto STDMETHODCALLTYPE ResettableSurface::GetDevice(::IDirect3DDevice9 **device) -> ::HRESULT
{
    return texture_->GetDevice(device);
}

auto STDMETHODCALLTYPE ResettableSurface::SetPrivateData(REFGUID guid, const void *data, ::DWORD size, ::DWORD flags)
    -> ::HRESULT
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }
    const auto result = inner->SetPrivateData(guid, data, size, flags);
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::GetPrivateData(REFGUID guid, void *data, ::DWORD *size) -> ::HRESULT
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }
    const auto result = inner->GetPrivateData(guid, data, size);
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::FreePrivateData(REFGUID guid) -> ::HRESULT
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }
    const auto result = inner->FreePrivateData(guid);
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::SetPriority(::DWORD priority) -> ::DWORD
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return 0u;
    }
    const auto result = inner->SetPriority(priority);
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::GetPriority() -> ::DWORD
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return 0u;
    }
    const auto result = inner->GetPriority();
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::PreLoad() -> void
{
    auto *inner = surface();
    if (inner != nullptr)
    {
        inner->PreLoad();
        inner->Release();
    }
}

auto STDMETHODCALLTYPE ResettableSurface::GetType() -> ::D3DRESOURCETYPE
{
    return D3DRTYPE_SURFACE;
}

auto STDMETHODCALLTYPE ResettableSurface::GetContainer(REFIID iid, void **container) -> ::HRESULT
{
    return texture_->QueryInterface(iid, container);
}

auto STDMETHODCALLTYPE ResettableSurface::GetDesc(::D3DSURFACE_DESC *desc) -> ::HRESULT
{
    return texture_->GetLevelDesc(level_, desc);
}

auto STDMETHODCALLTYPE ResettableSurface::LockRect(::D3DLOCKED_RECT *locked, const ::RECT *rect, ::DWORD flags)
    -> ::HRESULT
{
    return texture_->LockRect(level_, locked, rect, flags);
}

auto STDMETHODCALLTYPE ResettableSurface::UnlockRect() -> ::HRESULT
{
    return texture_->UnlockRect(level_);
}

auto STDMETHODCALLTYPE ResettableSurface::GetDC(::HDC *dc) -> ::HRESULT
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }
    const auto result = inner->GetDC(dc);
    inner->Release();
    return result;
}

auto STDMETHODCALLTYPE ResettableSurface::ReleaseDC(::HDC dc) -> ::HRESULT
{
    auto *inner = surface();
    if (inner == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }
    const auto result = inner->ReleaseDC(dc);
    inner->Release();
    return result;
}

auto snapshot_textures() -> Vector<ResettableTexture *>
{
    auto result = Vector<ResettableTexture *>{};
    auto lock = std::scoped_lock{registry_mutex};
    result.reserve(registered_textures.size());
    for (auto *texture : registered_textures)
    {
        // An entry is still in this set between Release observing zero and the
        // destructor erasing it. AddRef here would lift that count back off zero
        // and hand out a reference to an object already being destroyed.
        if (texture->try_add_ref())
        {
            result.push_back(texture);
        }
    }
    return result;
}

}

auto make_resettable_texture(
    ::IDirect3DDevice9 *device,
    CreateTextureFn create_texture,
    ::IDirect3DTexture9 *texture,
    const ResettableTextureShape &shape,
    TextureDestroyedFn destroyed) -> ::IDirect3DTexture9 *
{
    if (device == nullptr || create_texture == nullptr || texture == nullptr)
    {
        return nullptr;
    }
    return ResettableTexture::create(device, create_texture, texture, shape, destroyed);
}

auto acquire_bound_texture(::IDirect3DBaseTexture9 *texture) -> ::IDirect3DBaseTexture9 *
{
    if (texture == nullptr)
    {
        return nullptr;
    }

    auto *resettable = static_cast<ResettableTexture *>(nullptr);
    {
        auto lock = std::scoped_lock{registry_mutex};
        const auto found = registered_textures.find(reinterpret_cast<ResettableTexture *>(texture));
        // try_add_ref rather than AddRef: an entry stays in this set until the
        // destructor removes it, which is after the count has already reached
        // zero, so a plain AddRef here could resurrect a dying wrapper.
        if (found != registered_textures.end() && (*found)->try_add_ref())
        {
            resettable = *found;
        }
    }

    if (resettable == nullptr)
    {
        // Not one of ours. Take a reference anyway so the caller's Release is
        // balanced whichever branch was taken.
        texture->AddRef();
        return texture;
    }

    auto *inner = resettable->acquire_inner_for_binding();
    resettable->Release();
    return inner;
}

auto flush_staging_pool() -> std::uint64_t
{
    auto handles = lyrium::Vector<::IDirect3DTexture9 *>{};
    auto freed = std::uint64_t{};
    {
        auto lock = std::scoped_lock{staging_pool_mutex};
        freed = staging_pool().held_bytes();
        const auto taken = staging_pool().take_all();
        handles.assign(taken.begin(), taken.end());
    }
    // Released outside the lock: Release re-enters the driver.
    for (auto *staging : handles)
    {
        staging->Release();
    }
    return freed;
}

auto rewrap_resettable_texture(::IDirect3DBaseTexture9 *texture) -> ::IDirect3DBaseTexture9 *
{
    if (texture == nullptr)
    {
        return nullptr;
    }

    auto *resettable = static_cast<ResettableTexture *>(nullptr);
    {
        auto lock = std::scoped_lock{registry_mutex};
        const auto found = textures_by_inner.find(texture);
        if (found != textures_by_inner.end())
        {
            resettable = found->second;
            resettable->AddRef();
        }
    }
    if (resettable == nullptr)
    {
        return texture;
    }
    texture->Release();
    return resettable;
}

auto before_resettable_texture_reset(::IDirect3DDevice9 *device) -> void
{
    if (device != nullptr)
    {
        for (auto stage = ::DWORD{}; stage < 16u; ++stage)
        {
            device->SetTexture(stage, nullptr);
        }
        device->SetTexture(D3DDMAPSAMPLER, nullptr);
        device->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
        device->SetTexture(D3DVERTEXTEXTURESAMPLER1, nullptr);
        device->SetTexture(D3DVERTEXTEXTURESAMPLER2, nullptr);
        device->SetTexture(D3DVERTEXTEXTURESAMPLER3, nullptr);
    }

    auto textures = snapshot_textures();
    for (auto *texture : textures)
    {
        texture->release_inner();
        texture->Release();
    }
}

auto after_resettable_texture_reset() -> void
{
    auto textures = snapshot_textures();
    for (auto *texture : textures)
    {
        texture->restore_inner();
        texture->Release();
    }
}

}
