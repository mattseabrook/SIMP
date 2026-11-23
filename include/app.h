#ifndef SIMP_APP_H
#define SIMP_APP_H

#include <cstdint>
#include <vector>

#include <windows.h>

#include "canvas_render.h"

void initializeCanvasCaliper(HWND hwnd);
void shutdownCanvasCaliper();
void serviceCanvasBackgroundWork();
bool confirmCanvasCaliperClose(HWND owner);
void layoutCanvasCaliperChrome();
void loadCanvasCaliperConfig();
void saveCanvasCaliperConfig();
bool loadCanvasCaliperImagePath(HWND owner, const wchar_t *path, bool restore = false);
void openCanvasCaliperDocumentDialog(HWND owner);
void saveCanvasCaliperDocumentAsDialog(HWND owner);
void saveCanvasCaliperDocument(HWND owner);
void exportCanvasCaliperPngDialog(HWND owner);
void editCanvasCaliperHistory(HWND owner, bool redo);
void newCanvasCaliperImage();
void flushCanvasCaliperEdits();
void toggleCanvasView(HWND owner, UINT command);
bool handleCanvasCaliperWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT &result);
void updateCanvasCaliperMenu(HMENU menu);
CanvasRenderData &buildCanvasRenderData(std::uint32_t width, std::uint32_t height);
void showCanvasSettingsDialog(HWND owner, UINT command);

#endif
