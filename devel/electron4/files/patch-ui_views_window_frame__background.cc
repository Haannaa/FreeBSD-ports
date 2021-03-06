--- ui/views/window/frame_background.cc.orig	2019-03-15 06:26:45 UTC
+++ ui/views/window/frame_background.cc
@@ -122,7 +122,7 @@ void FrameBackground::PaintMaximized(gfx::Canvas* canv
                                      const View* view) const {
 // Fill the top with the frame color first so we have a constant background
 // for areas not covered by the theme image.
-#if defined(OS_LINUX) && !defined(OS_CHROMEOS)
+#if (defined(OS_LINUX) || defined(OS_BSD)) && !defined(OS_CHROMEOS)
   auto* native_theme = view->GetNativeTheme();
   ui::NativeTheme::ExtraParams params;
   params.frame_top_area.use_custom_frame = use_custom_frame_;
