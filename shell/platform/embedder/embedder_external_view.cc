// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_view.h"

#include "flutter/display_list/dl_builder.h"
#include "flutter/fml/trace_event.h"
#include "flutter/shell/common/dl_op_spy.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/GrRecordingContext.h"

#ifdef IMPELLER_SUPPORTS_RENDERING
#include "impeller/display_list/dl_dispatcher.h"  // nogncheck
#define ENABLE_EXPERIMENTAL_CANVAS false
#endif  // IMPELLER_SUPPORTS_RENDERING

namespace flutter {

static SkISize TransformedSurfaceSize(const SkISize& size,
                                      const SkMatrix& transformation) {
  const auto source_rect = SkRect::MakeWH(size.width(), size.height());
  const auto transformed_rect = transformation.mapRect(source_rect);
  return SkISize::Make(transformed_rect.width(), transformed_rect.height());
}

EmbedderExternalView::EmbedderExternalView(
    const SkISize& frame_size,
    const SkMatrix& surface_transformation)
    : EmbedderExternalView(frame_size, surface_transformation, {}, nullptr) {}

EmbedderExternalView::EmbedderExternalView(
    const SkISize& frame_size,
    const SkMatrix& surface_transformation,
    ViewIdentifier view_identifier,
    std::unique_ptr<EmbeddedViewParams> params)
    : render_surface_size_(
          TransformedSurfaceSize(frame_size, surface_transformation)),
      surface_transformation_(surface_transformation),
      view_identifier_(view_identifier),
      embedded_view_params_(std::move(params)),
      slice_(std::make_unique<DisplayListEmbedderViewSlice>(
          SkRect::Make(frame_size))) {}

EmbedderExternalView::~EmbedderExternalView() = default;

EmbedderExternalView::RenderTargetDescriptor
EmbedderExternalView::CreateRenderTargetDescriptor() const {
  return RenderTargetDescriptor(render_surface_size_);
}

DlCanvas* EmbedderExternalView::GetCanvas() {
  return slice_->canvas();
}

SkISize EmbedderExternalView::GetRenderSurfaceSize() const {
  return render_surface_size_;
}

bool EmbedderExternalView::IsRootView() const {
  return !HasPlatformView();
}

bool EmbedderExternalView::HasPlatformView() const {
  return view_identifier_.platform_view_id.has_value();
}

const DlRegion& EmbedderExternalView::GetDlRegion() const {
  return slice_->getRegion();
}

bool EmbedderExternalView::HasEngineRenderedContents() {
  if (has_engine_rendered_contents_.has_value()) {
    return has_engine_rendered_contents_.value();
  }
  TryEndRecording();
  DlOpSpy dl_op_spy;
  slice_->dispatch(dl_op_spy);
  has_engine_rendered_contents_ = dl_op_spy.did_draw() && !slice_->is_empty();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  return has_engine_rendered_contents_.value();
}

EmbedderExternalView::ViewIdentifier EmbedderExternalView::GetViewIdentifier()
    const {
  return view_identifier_;
}

const EmbeddedViewParams* EmbedderExternalView::GetEmbeddedViewParams() const {
  return embedded_view_params_.get();
}

bool EmbedderExternalView::Render(const EmbedderRenderTarget& render_target,
                                  bool clear_surface) {
  TRACE_EVENT0("flutter", "EmbedderExternalView::Render");
  TryEndRecording();
  FML_DCHECK(HasEngineRenderedContents())
      << "Unnecessarily asked to render into a render target when there was "
         "nothing to render.";

#ifdef IMPELLER_SUPPORTS_RENDERING
  auto* impeller_target = render_target.GetImpellerRenderTarget();
  if (impeller_target) {
    auto aiks_context = render_target.GetAiksContext();

    auto dl_builder = DisplayListBuilder();
    dl_builder.SetTransform(&surface_transformation_);
    slice_->render_into(&dl_builder);
    auto display_list = dl_builder.Build();

#if ENABLE_EXPERIMENTAL_CANVAS
    auto cull_rect =
        impeller::IRect::MakeSize(impeller_target->GetRenderTargetSize());
    SkIRect sk_cull_rect =
        SkIRect::MakeWH(cull_rect.GetWidth(), cull_rect.GetHeight());

    impeller::TextFrameDispatcher collector(aiks_context->GetContentContext(),
                                            impeller::Matrix());

    impeller::ExperimentalDlDispatcher impeller_dispatcher(
        aiks_context->GetContentContext(), *impeller_target,
        display_list->root_has_backdrop_filter(),
        display_list->max_root_blend_mode(), cull_rect);
    display_list->Dispatch(impeller_dispatcher, sk_cull_rect);
    impeller_dispatcher.FinishRecording();
    aiks_context->GetContentContext().GetTransientsBuffer().Reset();
    aiks_context->GetContentContext().GetLazyGlyphAtlas()->ResetTextFrames();

    return true;
#else
    auto dispatcher = impeller::DlDispatcher();
    dispatcher.drawDisplayList(display_list, 1);
    return aiks_context->Render(dispatcher.EndRecordingAsPicture(),
                                *impeller_target, /*reset_host_buffer=*/true);
#endif
  }
#endif  // IMPELLER_SUPPORTS_RENDERING

#if SLIMPELLER
  FML_LOG(FATAL) << "Impeller opt-out unavailable.";
  return false;
#else   // SLIMPELLER
  auto skia_surface = render_target.GetSkiaSurface();
  if (!skia_surface) {
    return false;
  }

  FML_DCHECK(render_target.GetRenderTargetSize() == render_surface_size_);

  auto canvas = skia_surface->getCanvas();
  if (!canvas) {
    return false;
  }
  DlSkCanvasAdapter dl_canvas(canvas);
  int restore_count = dl_canvas.GetSaveCount();
  dl_canvas.SetTransform(surface_transformation_);
  if (clear_surface) {
    dl_canvas.Clear(DlColor::kTransparent());
  }
  slice_->render_into(&dl_canvas);
  dl_canvas.RestoreToCount(restore_count);
  dl_canvas.Flush();
#endif  //  !SLIMPELLER

  return true;
}

void EmbedderExternalView::TryEndRecording() const {
  if (slice_->recording_ended()) {
    return;
  }
  slice_->end_recording();
}

}  // namespace flutter
