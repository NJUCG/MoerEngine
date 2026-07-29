#include "profile_viewer_ui/ProfileViewerModel.h"

#include <algorithm>
#include <limits>

namespace Moer {
namespace {

[[nodiscard]] std::uint64_t
MulDivFloor(std::uint64_t _value, std::uint32_t _numerator, std::uint32_t _denominator) noexcept {
    const std::uint64_t quotient  = _value / _denominator;
    const std::uint64_t remainder = _value % _denominator;
    return quotient * _numerator + (remainder * _numerator) / _denominator;
}

[[nodiscard]] std::uint64_t
ScaleCeilSaturated(std::uint64_t _value, std::uint32_t _numerator, std::uint32_t _denominator) noexcept {
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();

    const std::uint64_t quotient  = _value / _denominator;
    const std::uint64_t remainder = _value % _denominator;
    if (quotient > maximum / _numerator) {
        return maximum;
    }

    const std::uint64_t whole             = quotient * _numerator;
    const std::uint64_t remainder_product = remainder * _numerator;
    const std::uint64_t partial =
        remainder_product / _denominator + (remainder_product % _denominator != 0 ? 1 : 0);
    if (partial > maximum - whole) {
        return maximum;
    }
    return whole + partial;
}

[[nodiscard]] std::uint64_t SignedMagnitude(std::int64_t _value) noexcept {
    if (_value >= 0) {
        return static_cast<std::uint64_t>(_value);
    }
    return static_cast<std::uint64_t>(-(_value + 1)) + 1;
}

} // namespace

std::uint64_t ProfileViewerMapFraction(
    std::uint64_t _begin,
    std::uint64_t _end,
    std::uint32_t _numerator,
    std::uint32_t _denominator
) noexcept {
    if (_end <= _begin || _denominator == 0 || _numerator > _denominator || _numerator == 0) {
        return _begin;
    }
    if (_numerator == _denominator) {
        return _end;
    }

    return _begin + MulDivFloor(_end - _begin, _numerator, _denominator);
}

bool ProfileViewerSelection::Valid() const noexcept {
    if (kind == EProfileViewerSelectionKind::None ||
        published_generation == ProfileDump::kInvalidProfileDocumentGeneration ||
        source_scope_index == ProfileDump::kInvalidSessionIndex ||
        timeline_track_index == ProfileDump::kInvalidSessionIndex || begin > end) {
        return false;
    }
    if (kind == EProfileViewerSelectionKind::CpuScope) {
        return axis_index == 0 && frame_id == 0;
    }
    return kind == EProfileViewerSelectionKind::GpuScope && axis_index != 0 &&
           axis_index != ProfileDump::kInvalidTimelineAxis;
}

EProfileViewerPublicationUpdate
ProfileViewerModel::ObserveSnapshot(const ProfileDump::ProfileDocumentLoaderSnapshot& _snapshot) noexcept {
    const std::uint64_t observed_generation = _snapshot.published_generation;
    if (observed_generation == ProfileDump::kInvalidProfileDocumentGeneration) {
        if (_snapshot.document) {
            return EProfileViewerPublicationUpdate::Invalid;
        }
    } else if (!_snapshot.document || _snapshot.document->request_generation != observed_generation ||
               !_snapshot.document->Valid()) {
        return EProfileViewerPublicationUpdate::Invalid;
    }

    if (observed_generation == published_generation_) {
        return EProfileViewerPublicationUpdate::Unchanged;
    }

    published_generation_ = observed_generation;
    ResetPublicationState();
    return EProfileViewerPublicationUpdate::Changed;
}

std::uint64_t ProfileViewerModel::PublishedGeneration() const noexcept {
    return published_generation_;
}

ProfileViewerViewport ProfileViewerModel::CpuViewport() const noexcept {
    return cpu_viewport_;
}

bool ProfileViewerModel::FitCpu(std::uint64_t _domain_end) noexcept {
    return published_generation_ != ProfileDump::kInvalidProfileDocumentGeneration &&
           FitViewport(cpu_viewport_, _domain_end);
}

bool ProfileViewerModel::ZoomCpu(
    std::uint32_t _anchor_numerator,
    std::uint32_t _anchor_denominator,
    std::uint32_t _scale_numerator,
    std::uint32_t _scale_denominator
) noexcept {
    return ZoomViewport(
        cpu_viewport_, _anchor_numerator, _anchor_denominator, _scale_numerator, _scale_denominator
    );
}

bool ProfileViewerModel::PanCpu(std::int64_t _delta) noexcept {
    return PanViewport(cpu_viewport_, _delta);
}

bool ProfileViewerModel::FocusCpu(std::uint64_t _begin, std::uint64_t _end, std::uint64_t _margin) noexcept {
    return FocusViewport(cpu_viewport_, _begin, _end, _margin);
}

std::optional<ProfileViewerViewport> ProfileViewerModel::FindGpuViewport(ProfileViewerGpuViewportKey _key
) const noexcept {
    const GpuViewportSlot* slot = FindGpuViewportSlot(_key);
    if (slot == nullptr) {
        return std::nullopt;
    }
    return slot->viewport;
}

bool ProfileViewerModel::FitGpu(ProfileViewerGpuViewportKey _key, std::uint64_t _domain_end) noexcept {
    if (published_generation_ == ProfileDump::kInvalidProfileDocumentGeneration || !IsValidGpuKey(_key) ||
        _domain_end == 0) {
        return false;
    }

    GpuViewportSlot* slot = AcquireGpuViewportSlot(_key);
    return slot != nullptr && FitViewport(slot->viewport, _domain_end);
}

bool ProfileViewerModel::ZoomGpu(
    ProfileViewerGpuViewportKey _key,
    std::uint32_t               _anchor_numerator,
    std::uint32_t               _anchor_denominator,
    std::uint32_t               _scale_numerator,
    std::uint32_t               _scale_denominator
) noexcept {
    GpuViewportSlot* slot = FindGpuViewportSlot(_key);
    return slot != nullptr &&
           ZoomViewport(
               slot->viewport, _anchor_numerator, _anchor_denominator, _scale_numerator, _scale_denominator
           );
}

bool ProfileViewerModel::PanGpu(ProfileViewerGpuViewportKey _key, std::int64_t _delta) noexcept {
    GpuViewportSlot* slot = FindGpuViewportSlot(_key);
    return slot != nullptr && PanViewport(slot->viewport, _delta);
}

bool ProfileViewerModel::FocusGpu(
    ProfileViewerGpuViewportKey _key,
    std::uint64_t               _begin,
    std::uint64_t               _end,
    std::uint64_t               _margin
) noexcept {
    GpuViewportSlot* slot = FindGpuViewportSlot(_key);
    return slot != nullptr && FocusViewport(slot->viewport, _begin, _end, _margin);
}

bool ProfileViewerModel::SelectCpu(
    std::uint64_t _timeline_track_index,
    std::uint64_t _source_scope_index,
    std::uint64_t _begin,
    std::uint64_t _end
) noexcept {
    if (published_generation_ == ProfileDump::kInvalidProfileDocumentGeneration ||
        _timeline_track_index == ProfileDump::kInvalidSessionIndex ||
        _source_scope_index == ProfileDump::kInvalidSessionIndex ||
        !ContainsInterval(cpu_viewport_, _begin, _end)) {
        return false;
    }

    selection_ = {
        .kind                 = EProfileViewerSelectionKind::CpuScope,
        .published_generation = published_generation_,
        .source_scope_index   = _source_scope_index,
        .timeline_track_index = _timeline_track_index,
        .axis_index           = 0,
        .frame_id             = 0,
        .begin                = _begin,
        .end                  = _end,
    };
    return true;
}

bool ProfileViewerModel::SelectGpu(
    ProfileViewerGpuViewportKey _key,
    std::uint64_t               _timeline_track_index,
    std::uint64_t               _source_scope_index,
    std::uint64_t               _begin,
    std::uint64_t               _end
) noexcept {
    const GpuViewportSlot* slot = FindGpuViewportSlot(_key);
    if (published_generation_ == ProfileDump::kInvalidProfileDocumentGeneration ||
        _timeline_track_index == ProfileDump::kInvalidSessionIndex ||
        _source_scope_index == ProfileDump::kInvalidSessionIndex || slot == nullptr ||
        !ContainsInterval(slot->viewport, _begin, _end)) {
        return false;
    }

    selection_ = {
        .kind                 = EProfileViewerSelectionKind::GpuScope,
        .published_generation = published_generation_,
        .source_scope_index   = _source_scope_index,
        .timeline_track_index = _timeline_track_index,
        .axis_index           = _key.axis_index,
        .frame_id             = _key.frame_id,
        .begin                = _begin,
        .end                  = _end,
    };
    return true;
}

ProfileViewerSelection ProfileViewerModel::Selection() const noexcept {
    return selection_;
}

void ProfileViewerModel::ClearSelection() noexcept {
    selection_ = {};
}

std::size_t ProfileViewerModel::ActiveGpuViewportCount() const noexcept {
    return active_gpu_viewport_count_;
}

bool ProfileViewerModel::IsValidGpuKey(ProfileViewerGpuViewportKey _key) noexcept {
    return _key.axis_index != 0 && _key.axis_index != ProfileDump::kInvalidTimelineAxis;
}

bool ProfileViewerModel::FitViewport(ProfileViewerViewport& _viewport, std::uint64_t _domain_end) noexcept {
    if (_domain_end == 0) {
        return false;
    }
    _viewport = {
        .valid      = true,
        .domain_end = _domain_end,
        .view_begin = 0,
        .view_end   = _domain_end,
    };
    return true;
}

bool ProfileViewerModel::ZoomViewport(
    ProfileViewerViewport& _viewport,
    std::uint32_t          _anchor_numerator,
    std::uint32_t          _anchor_denominator,
    std::uint32_t          _scale_numerator,
    std::uint32_t          _scale_denominator
) noexcept {
    if (!_viewport.valid || _viewport.domain_end == 0 || _viewport.view_begin >= _viewport.view_end ||
        _viewport.view_end > _viewport.domain_end || _anchor_denominator == 0 ||
        _anchor_numerator > _anchor_denominator || _scale_numerator == 0 || _scale_denominator == 0) {
        return false;
    }

    const std::uint64_t old_span = _viewport.view_end - _viewport.view_begin;
    const std::uint64_t new_span = std::clamp(
        ScaleCeilSaturated(old_span, _scale_numerator, _scale_denominator),
        std::uint64_t{1},
        _viewport.domain_end
    );
    const std::uint64_t old_anchor_offset = MulDivFloor(old_span, _anchor_numerator, _anchor_denominator);
    const std::uint64_t anchor            = _viewport.view_begin + old_anchor_offset;
    const std::uint64_t new_anchor_offset = MulDivFloor(new_span, _anchor_numerator, _anchor_denominator);

    std::uint64_t       new_begin     = anchor >= new_anchor_offset ? anchor - new_anchor_offset : 0;
    const std::uint64_t maximum_begin = _viewport.domain_end - new_span;
    new_begin                         = std::min(new_begin, maximum_begin);
    _viewport.view_begin              = new_begin;
    _viewport.view_end                = new_begin + new_span;
    return true;
}

bool ProfileViewerModel::PanViewport(ProfileViewerViewport& _viewport, std::int64_t _delta) noexcept {
    if (!_viewport.valid || _viewport.domain_end == 0 || _viewport.view_begin >= _viewport.view_end ||
        _viewport.view_end > _viewport.domain_end) {
        return false;
    }

    const std::uint64_t span          = _viewport.view_end - _viewport.view_begin;
    const std::uint64_t maximum_begin = _viewport.domain_end - span;
    const std::uint64_t magnitude     = SignedMagnitude(_delta);
    std::uint64_t       new_begin     = _viewport.view_begin;
    if (_delta >= 0) {
        new_begin = magnitude >= maximum_begin - new_begin ? maximum_begin : new_begin + magnitude;
    } else {
        new_begin = magnitude >= new_begin ? 0 : new_begin - magnitude;
    }

    _viewport.view_begin = new_begin;
    _viewport.view_end   = new_begin + span;
    return true;
}

bool ProfileViewerModel::FocusViewport(
    ProfileViewerViewport& _viewport,
    std::uint64_t          _begin,
    std::uint64_t          _end,
    std::uint64_t          _margin
) noexcept {
    if (!_viewport.valid || _viewport.domain_end == 0 || _viewport.view_begin >= _viewport.view_end ||
        _viewport.view_end > _viewport.domain_end) {
        return false;
    }

    const std::uint64_t focus_begin = std::min(_begin, _viewport.domain_end - 1);
    std::uint64_t       focus_end   = std::min(_end, _viewport.domain_end);
    if (focus_end <= focus_begin) {
        focus_end = focus_begin + 1;
    }

    const std::uint64_t view_begin = _margin > focus_begin ? 0 : focus_begin - _margin;
    const std::uint64_t remaining  = _viewport.domain_end - focus_end;
    const std::uint64_t view_end   = _margin >= remaining ? _viewport.domain_end : focus_end + _margin;
    _viewport.view_begin           = view_begin;
    _viewport.view_end             = view_end;
    return true;
}

bool ProfileViewerModel::ContainsInterval(
    const ProfileViewerViewport& _viewport,
    std::uint64_t                _begin,
    std::uint64_t                _end
) noexcept {
    return _viewport.valid && _viewport.domain_end != 0 && _viewport.view_begin < _viewport.view_end &&
           _viewport.view_end <= _viewport.domain_end && _begin <= _end && _begin < _viewport.domain_end &&
           _end <= _viewport.domain_end;
}

ProfileViewerModel::GpuViewportSlot* ProfileViewerModel::FindGpuViewportSlot(ProfileViewerGpuViewportKey _key
) noexcept {
    if (!IsValidGpuKey(_key)) {
        return nullptr;
    }
    for (GpuViewportSlot& slot : gpu_viewports_) {
        if (slot.occupied && slot.key == _key) {
            return &slot;
        }
    }
    return nullptr;
}

const ProfileViewerModel::GpuViewportSlot*
ProfileViewerModel::FindGpuViewportSlot(ProfileViewerGpuViewportKey _key) const noexcept {
    if (!IsValidGpuKey(_key)) {
        return nullptr;
    }
    for (const GpuViewportSlot& slot : gpu_viewports_) {
        if (slot.occupied && slot.key == _key) {
            return &slot;
        }
    }
    return nullptr;
}

ProfileViewerModel::GpuViewportSlot*
ProfileViewerModel::AcquireGpuViewportSlot(ProfileViewerGpuViewportKey _key) noexcept {
    if (GpuViewportSlot* existing = FindGpuViewportSlot(_key)) {
        return existing;
    }

    for (GpuViewportSlot& slot : gpu_viewports_) {
        if (!slot.occupied) {
            slot = {
                .occupied = true,
                .key      = _key,
                .viewport = {},
            };
            ++active_gpu_viewport_count_;
            return &slot;
        }
    }

    GpuViewportSlot& slot = gpu_viewports_[next_gpu_eviction_];
    next_gpu_eviction_    = (next_gpu_eviction_ + 1) % gpu_viewports_.size();
    slot                  = {
                         .occupied = true,
                         .key      = _key,
                         .viewport = {},
    };
    return &slot;
}

void ProfileViewerModel::ResetPublicationState() noexcept {
    cpu_viewport_ = {};
    for (GpuViewportSlot& slot : gpu_viewports_) {
        slot = {};
    }
    active_gpu_viewport_count_ = 0;
    next_gpu_eviction_         = 0;
    selection_                 = {};
}

} // namespace Moer
