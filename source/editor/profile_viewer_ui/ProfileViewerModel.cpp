#include "profile_viewer_ui/ProfileViewerModel.h"
#include "misc/Types.h"
#include <algorithm>
#include <iterator>
#include <limits>

namespace Moer {
namespace {

[[nodiscard]] uint64
MulDivFloor(uint64 _value, uint32 _numerator, uint32 _denominator) noexcept {
    const uint64 quotient  = _value / _denominator;
    const uint64 remainder = _value % _denominator;
    return quotient * _numerator + (remainder * _numerator) / _denominator;
}

[[nodiscard]] uint64
ScaleCeilSaturated(uint64 _value, uint32 _numerator, uint32 _denominator) noexcept {
    constexpr uint64 maximum = std::numeric_limits<uint64>::max();

    const uint64 quotient  = _value / _denominator;
    const uint64 remainder = _value % _denominator;
    if (quotient > maximum / _numerator) {
        return maximum;
    }

    const uint64 whole             = quotient * _numerator;
    const uint64 remainder_product = remainder * _numerator;
    const uint64 partial =
        remainder_product / _denominator + (remainder_product % _denominator != 0 ? 1 : 0);
    if (partial > maximum - whole) {
        return maximum;
    }
    return whole + partial;
}

[[nodiscard]] uint64 SignedMagnitude(int64 _value) noexcept {
    if (_value >= 0) {
        return static_cast<uint64>(_value);
    }
    return static_cast<uint64>(-(_value + 1)) + 1;
}

enum class EGpuFrameChoiceSearch : uint8 {
    AtOrAfter = 0,
    After,
    Before,
    AtOrBefore,
};

template<typename Iterator>
[[nodiscard]] Iterator FindFrameInSortedRange(
    Iterator              _begin,
    Iterator              _end,
    uint64                _frame_id,
    EGpuFrameChoiceSearch _search
) noexcept {
    const auto less = [](const auto& _frame, uint64 _value) {
        return _frame.frame_id < _value;
    };
    const auto reverse_less = [](uint64 _value, const auto& _frame) {
        return _value < _frame.frame_id;
    };

    switch (_search) {
        case EGpuFrameChoiceSearch::AtOrAfter:
            return std::lower_bound(_begin, _end, _frame_id, less);
        case EGpuFrameChoiceSearch::After:
            return std::upper_bound(_begin, _end, _frame_id, reverse_less);
        case EGpuFrameChoiceSearch::Before: {
            const Iterator found = std::lower_bound(_begin, _end, _frame_id, less);
            return found == _begin ? _end : std::prev(found);
        }
        case EGpuFrameChoiceSearch::AtOrBefore: {
            const Iterator found = std::upper_bound(_begin, _end, _frame_id, reverse_less);
            return found == _begin ? _end : std::prev(found);
        }
    }
    return _end;
}

void MergeGpuFrameChoice(
    std::optional<ProfileViewerGpuFrameChoice>& _best,
    ProfileViewerGpuFrameChoice                 _candidate,
    EGpuFrameChoiceSearch                       _search
) noexcept {
    if (!_best) {
        _best = _candidate;
        return;
    }
    if (_candidate.frame_id == _best->frame_id) {
        _best->has_frame_record = _best->has_frame_record || _candidate.has_frame_record;
        return;
    }

    const bool prefer_lower =
        _search == EGpuFrameChoiceSearch::AtOrAfter || _search == EGpuFrameChoiceSearch::After;
    if ((prefer_lower && _candidate.frame_id < _best->frame_id) ||
        (!prefer_lower && _candidate.frame_id > _best->frame_id)) {
        _best = _candidate;
    }
}

template<typename Iterator, typename HasFrameRecord>
void ConsiderGpuFrameRange(
    Iterator                                    _begin,
    Iterator                                    _end,
    uint64                                      _frame_id,
    EGpuFrameChoiceSearch                       _search,
    HasFrameRecord                              _has_frame_record,
    std::optional<ProfileViewerGpuFrameChoice>& _best
) noexcept {
    const Iterator found = FindFrameInSortedRange(_begin, _end, _frame_id, _search);
    if (found != _end) {
        MergeGpuFrameChoice(
            _best,
            ProfileViewerGpuFrameChoice{
                .frame_id         = found->frame_id,
                .has_frame_record = _has_frame_record(*found),
            },
            _search
        );
    }
}

[[nodiscard]] std::optional<ProfileViewerGpuFrameChoice> FindProfileViewerGpuFrame(
    std::span<const ProfileDump::GpuTimelineFrameRef>  _recorded_frames,
    std::span<const ProfileDump::GpuTimelineAxisFrame> _axis_frames,
    uint64                                             _frame_id,
    EGpuFrameChoiceSearch                              _search
) noexcept {
    std::optional<ProfileViewerGpuFrameChoice> best;
    ConsiderGpuFrameRange(
        _recorded_frames.begin(),
        _recorded_frames.end(),
        _frame_id,
        _search,
        [](const ProfileDump::GpuTimelineFrameRef&) {
            return true;
        },
        best
    );

    // GpuAxisFrames are sorted by (axis_index, frame_id). Binary-search each
    // physical-axis group that the UI can render (indices 1..256). This keeps
    // chooser and tab discovery on the same contract. Work is
    // O(renderable_axis_count * log(frame_count)) with no allocation and no
    // scan proportional to the scope/axis-frame count.
    auto group_begin = _axis_frames.begin();
    while (group_begin != _axis_frames.end()) {
        const uint32 axis_index = group_begin->axis_index;
        const auto   group_end  = std::upper_bound(
            group_begin,
            _axis_frames.end(),
            axis_index,
            [](uint32 _value, const ProfileDump::GpuTimelineAxisFrame& _frame) {
                return _value < _frame.axis_index;
            }
        );
        if (axis_index > kProfileViewerGpuRenderableAxisMax) {
            break;
        }
        if (axis_index != 0) {
            ConsiderGpuFrameRange(
                group_begin,
                group_end,
                _frame_id,
                _search,
                [](const ProfileDump::GpuTimelineAxisFrame& _frame) {
                    return _frame.has_frame_record;
                },
                best
            );
        }
        group_begin = group_end;
    }
    return best;
}

} // namespace

uint64 ProfileViewerMapFraction(
    uint64 _begin,
    uint64 _end,
    uint32 _numerator,
    uint32 _denominator
) noexcept {
    if (_end <= _begin || _denominator == 0 || _numerator > _denominator || _numerator == 0) {
        return _begin;
    }
    if (_numerator == _denominator) {
        return _end;
    }

    return _begin + MulDivFloor(_end - _begin, _numerator, _denominator);
}

std::optional<ProfileViewerGpuFrameChoice> FindProfileViewerGpuFrameAtOrAfter(
    std::span<const ProfileDump::GpuTimelineFrameRef>  _recorded_frames,
    std::span<const ProfileDump::GpuTimelineAxisFrame> _axis_frames,
    uint64                                             _frame_id
) noexcept {
    return FindProfileViewerGpuFrame(
        _recorded_frames, _axis_frames, _frame_id, EGpuFrameChoiceSearch::AtOrAfter
    );
}

std::optional<ProfileViewerGpuFrameChoice> FindProfileViewerGpuFrameAfter(
    std::span<const ProfileDump::GpuTimelineFrameRef>  _recorded_frames,
    std::span<const ProfileDump::GpuTimelineAxisFrame> _axis_frames,
    uint64                                             _frame_id
) noexcept {
    return FindProfileViewerGpuFrame(_recorded_frames, _axis_frames, _frame_id, EGpuFrameChoiceSearch::After);
}

std::optional<ProfileViewerGpuFrameChoice> FindProfileViewerGpuFrameBefore(
    std::span<const ProfileDump::GpuTimelineFrameRef>  _recorded_frames,
    std::span<const ProfileDump::GpuTimelineAxisFrame> _axis_frames,
    uint64                                             _frame_id
) noexcept {
    return FindProfileViewerGpuFrame(
        _recorded_frames, _axis_frames, _frame_id, EGpuFrameChoiceSearch::Before
    );
}

std::optional<ProfileViewerGpuFrameChoice> FindProfileViewerGpuFrameAtOrBefore(
    std::span<const ProfileDump::GpuTimelineFrameRef>  _recorded_frames,
    std::span<const ProfileDump::GpuTimelineAxisFrame> _axis_frames,
    uint64                                             _frame_id
) noexcept {
    return FindProfileViewerGpuFrame(
        _recorded_frames, _axis_frames, _frame_id, EGpuFrameChoiceSearch::AtOrBefore
    );
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
ProfileViewerModel::InspectSnapshot(const ProfileDump::ProfileDocumentLoaderSnapshot& _snapshot
) const noexcept {
    const uint64 observed_generation = _snapshot.published_generation;
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
    return EProfileViewerPublicationUpdate::Changed;
}

EProfileViewerPublicationUpdate
ProfileViewerModel::ObserveSnapshot(const ProfileDump::ProfileDocumentLoaderSnapshot& _snapshot) noexcept {
    const EProfileViewerPublicationUpdate update = InspectSnapshot(_snapshot);
    if (update != EProfileViewerPublicationUpdate::Changed) {
        return update;
    }
    const uint64 observed_generation        = _snapshot.published_generation;
    published_generation_                   = observed_generation;
    ResetPublicationState();
    return update;
}

uint64 ProfileViewerModel::PublishedGeneration() const noexcept {
    return published_generation_;
}

ProfileViewerViewport ProfileViewerModel::CpuViewport() const noexcept {
    return cpu_viewport_;
}

bool ProfileViewerModel::FitCpu(uint64 _domain_end) noexcept {
    return published_generation_ != ProfileDump::kInvalidProfileDocumentGeneration &&
           FitViewport(cpu_viewport_, _domain_end);
}

bool ProfileViewerModel::ZoomCpu(
    uint32 _anchor_numerator,
    uint32 _anchor_denominator,
    uint32 _scale_numerator,
    uint32 _scale_denominator
) noexcept {
    return ZoomViewport(
        cpu_viewport_, _anchor_numerator, _anchor_denominator, _scale_numerator, _scale_denominator
    );
}

bool ProfileViewerModel::PanCpu(int64 _delta) noexcept {
    return PanViewport(cpu_viewport_, _delta);
}

bool ProfileViewerModel::FocusCpu(uint64 _begin, uint64 _end, uint64 _margin) noexcept {
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

bool ProfileViewerModel::FitGpu(ProfileViewerGpuViewportKey _key, uint64 _domain_end) noexcept {
    if (published_generation_ == ProfileDump::kInvalidProfileDocumentGeneration || !IsValidGpuKey(_key) ||
        _domain_end == 0) {
        return false;
    }

    GpuViewportSlot* slot = AcquireGpuViewportSlot(_key);
    return slot != nullptr && FitViewport(slot->viewport, _domain_end);
}

bool ProfileViewerModel::ZoomGpu(
    ProfileViewerGpuViewportKey _key,
    uint32                      _anchor_numerator,
    uint32                      _anchor_denominator,
    uint32                      _scale_numerator,
    uint32                      _scale_denominator
) noexcept {
    GpuViewportSlot* slot = FindGpuViewportSlot(_key);
    return slot != nullptr &&
           ZoomViewport(
               slot->viewport, _anchor_numerator, _anchor_denominator, _scale_numerator, _scale_denominator
           );
}

bool ProfileViewerModel::PanGpu(ProfileViewerGpuViewportKey _key, int64 _delta) noexcept {
    GpuViewportSlot* slot = FindGpuViewportSlot(_key);
    return slot != nullptr && PanViewport(slot->viewport, _delta);
}

bool ProfileViewerModel::FocusGpu(
    ProfileViewerGpuViewportKey _key,
    uint64                      _begin,
    uint64                      _end,
    uint64                      _margin
) noexcept {
    GpuViewportSlot* slot = FindGpuViewportSlot(_key);
    return slot != nullptr && FocusViewport(slot->viewport, _begin, _end, _margin);
}

bool ProfileViewerModel::SelectCpu(
    uint64 _timeline_track_index,
    uint64 _source_scope_index,
    uint64 _begin,
    uint64 _end
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
    uint64                      _timeline_track_index,
    uint64                      _source_scope_index,
    uint64                      _begin,
    uint64                      _end
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

bool ProfileViewerModel::FitViewport(ProfileViewerViewport& _viewport, uint64 _domain_end) noexcept {
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
    uint32                 _anchor_numerator,
    uint32                 _anchor_denominator,
    uint32                 _scale_numerator,
    uint32                 _scale_denominator
) noexcept {
    if (!_viewport.valid || _viewport.domain_end == 0 || _viewport.view_begin >= _viewport.view_end ||
        _viewport.view_end > _viewport.domain_end || _anchor_denominator == 0 ||
        _anchor_numerator > _anchor_denominator || _scale_numerator == 0 || _scale_denominator == 0) {
        return false;
    }

    const uint64 old_span = _viewport.view_end - _viewport.view_begin;
    const uint64 new_span = std::clamp(
        ScaleCeilSaturated(old_span, _scale_numerator, _scale_denominator),
        uint64{1},
        _viewport.domain_end
    );
    const uint64 old_anchor_offset = MulDivFloor(old_span, _anchor_numerator, _anchor_denominator);
    const uint64 anchor            = _viewport.view_begin + old_anchor_offset;
    const uint64 new_anchor_offset = MulDivFloor(new_span, _anchor_numerator, _anchor_denominator);

    uint64       new_begin            = anchor >= new_anchor_offset ? anchor - new_anchor_offset : 0;
    const uint64 maximum_begin        = _viewport.domain_end - new_span;
    new_begin                         = std::min(new_begin, maximum_begin);
    _viewport.view_begin              = new_begin;
    _viewport.view_end                = new_begin + new_span;
    return true;
}

bool ProfileViewerModel::PanViewport(ProfileViewerViewport& _viewport, int64 _delta) noexcept {
    if (!_viewport.valid || _viewport.domain_end == 0 || _viewport.view_begin >= _viewport.view_end ||
        _viewport.view_end > _viewport.domain_end) {
        return false;
    }

    const uint64 span          = _viewport.view_end - _viewport.view_begin;
    const uint64 maximum_begin = _viewport.domain_end - span;
    const uint64 magnitude     = SignedMagnitude(_delta);
    uint64       new_begin     = _viewport.view_begin;
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
    uint64                 _begin,
    uint64                 _end,
    uint64                 _margin
) noexcept {
    if (!_viewport.valid || _viewport.domain_end == 0 || _viewport.view_begin >= _viewport.view_end ||
        _viewport.view_end > _viewport.domain_end) {
        return false;
    }

    const uint64 focus_begin = std::min(_begin, _viewport.domain_end - 1);
    uint64       focus_end   = std::min(_end, _viewport.domain_end);
    if (focus_end <= focus_begin) {
        focus_end = focus_begin + 1;
    }

    const uint64 view_begin        = _margin > focus_begin ? 0 : focus_begin - _margin;
    const uint64 remaining         = _viewport.domain_end - focus_end;
    const uint64 view_end          = _margin >= remaining ? _viewport.domain_end : focus_end + _margin;
    _viewport.view_begin           = view_begin;
    _viewport.view_end             = view_end;
    return true;
}

bool ProfileViewerModel::ContainsInterval(
    const ProfileViewerViewport& _viewport,
    uint64                       _begin,
    uint64                       _end
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
