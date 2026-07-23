#pragma once
// Boundary between the IDA-side dock glue (ida_dock.cpp, includes IDA headers,
// NO Qt) and the Qt-side widget builder (ida_views.cpp, includes Qt, NO IDA
// headers). They must never share a translation unit: IDA's pro.h and Qt's
// qbytearrayalgorithms.h both define qstrlen/qsnprintf/... as unconditional
// inline functions and collide (a real clash since IDA 9.x moved to stock Qt6).
//
// Neither side includes the other's toolkit headers — only this plain header.

// Number of dock views and their titles (defined in ida_views.cpp).
constexpr int SMD_DGX_VIEW_COUNT = 8;
extern const char* const smd_dgx_view_titles[SMD_DGX_VIEW_COUNT];

// Qt side (ida_views.cpp) — called by the IDA side:
//   attach: wrap an IDA TWidget (passed as its QWidget* as void*) with a layout
//           and the view widget for index `idx`.
void smd_dgx_view_attach(int idx, void* twidget_as_qwidget);
//   detach_all: stop the refresh timer and drop widget pointers (on term).
void smd_dgx_view_detach_all();

// IDA side (ida_dock.cpp) — called by the plugmod:
void smd_dgx_register_views();
void smd_dgx_unregister_views();
