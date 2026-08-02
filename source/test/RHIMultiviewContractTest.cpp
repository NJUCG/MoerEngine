#include "rhi/RHIMultiview.h"
#include "rhi/RHIResource.h"

#include <iostream>
#include <stdexcept>

using namespace Moer::Render;

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void ViewMaskShapeIsExplicit() {
    Expect(Multiview::RequiredViewCount(0u) == 0u, "zero mask requires a view");
    Expect(Multiview::RequiredViewCount(0x3fu) == 6u, "cube mask does not require six views");
    Expect(Multiview::RequiredViewCount(0x40u) == 7u, "highest set bit was not preserved");
    Expect(Multiview::RequiredViewCount(0x21u) == 6u, "sparse view mask was treated as popcount");
}

void CapabilityFailsClosed() {
    Expect(!Multiview::SupportsViewCount(false, 32u, 6u), "disabled feature was accepted");
    Expect(!Multiview::SupportsViewCount(true, 5u, 6u), "undersized view limit was accepted");
    Expect(Multiview::SupportsViewCount(true, 6u, 6u), "six-view capability was rejected");
    Expect(!Multiview::SupportsViewCount(true, 6u, 0u), "zero-view request was accepted");
}

void AttachmentCoverageUsesViewRelativeLayers() {
    Expect(Multiview::IsAttachmentRangeValid(0u, 6u, 6u), "whole cube range was rejected");
    Expect(Multiview::IsAttachmentRangeValid(1u, 6u, 7u), "nonzero base range was rejected");
    Expect(!Multiview::IsAttachmentRangeValid(2u, 6u, 7u), "out-of-bounds range was accepted");
    Expect(!Multiview::IsAttachmentRangeValid(0u, 0u, 6u), "empty range was accepted");

    Expect(Multiview::AttachmentCoversViewMask(6u, 0x3fu), "six layers do not cover cube mask");
    Expect(!Multiview::AttachmentCoversViewMask(5u, 0x3fu), "five layers cover cube mask");
    Expect(Multiview::AttachmentCoversViewMask(6u, 0x21u), "sparse mask coverage is incorrect");
    Expect(!Multiview::AttachmentCoversViewMask(5u, 0x21u), "sparse mask ignored its highest view");
}

void RhiPayloadCarriesIndependentMasksAndRanges() {
    GfxPsoCreateInfo pipeline_info(RHIRasterizeInfo::Preset(), {}, {});
    pipeline_info.SetViewMask(0x3fu);
    Expect(pipeline_info.view_mask == 0x3fu, "pipeline view mask was not stored");
    Expect(pipeline_info.multi_view_count == 1u, "view mask changed viewport count");

    TextureView depth_view{};
    depth_view.texture     = nullptr;
    depth_view.mip_level   = 2u;
    depth_view.array_layer = 1u;
    depth_view.num_array   = 6u;
    const DepthAttachment depth_attachment(depth_view);
    Expect(
        depth_attachment.mip_level == 2u && depth_attachment.array_layer == 1u &&
            depth_attachment.array_count == 6u,
        "depth attachment dropped its texture-view range"
    );

    RenderPassInfo pass_info{};
    pass_info.view_mask = 0x3fu;
    Expect(
        Multiview::PipelineMatchesRenderPass(pipeline_info.view_mask, pass_info.view_mask),
        "matching pipeline and render-pass masks were rejected"
    );
    pass_info.view_mask = 0x1u;
    Expect(
        !Multiview::PipelineMatchesRenderPass(pipeline_info.view_mask, pass_info.view_mask),
        "pipeline/render-pass mismatch was accepted"
    );
}

} // namespace

int main() {
    try {
        ViewMaskShapeIsExplicit();
        CapabilityFailsClosed();
        AttachmentCoverageUsesViewRelativeLayers();
        RhiPayloadCarriesIndependentMasksAndRanges();
        std::cout << "RHIMultiviewContract: all checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHIMultiviewContract: " << error.what() << '\n';
        return 1;
    }
}
