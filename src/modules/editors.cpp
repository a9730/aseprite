/* ASEPRITE
 * Copyright (C) 2001-2012  David Capello
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include "modules/editors.h"

#include "app.h"
#include "document_wrappers.h"
#include "ini_file.h"
#include "modules/gui.h"
#include "modules/palettes.h"
#include "raster/image.h"
#include "raster/sprite.h"
#include "skin/skin_theme.h"
#include "ui/gui.h"
#include "ui_context.h"
#include "util/misc.h"
#include "widgets/editor/editor.h"
#include "widgets/editor/editor_customization_delegate.h"
#include "widgets/editor/editor_view.h"
#include "widgets/main_window.h"
#include "widgets/popup_window_pin.h"
#include "widgets/status_bar.h"
#include "widgets/toolbar.h"

#include <algorithm>
#include <vector>

using namespace ui;

#define FIXUP_TOP_WINDOW()                             \
  App::instance()->getMainWindow()->remap_window();    \
  App::instance()->getMainWindow()->invalidate();

class EditorItem
{
public:
  enum Type { Normal, Mini };

  EditorItem(Editor* editor, Type type)
    : m_editor(editor)
    , m_type(type)
  { }

  Editor* getEditor() const { return m_editor; }
  Type getType() const { return m_type; }

private:
  Editor* m_editor;
  Type m_type;
};

typedef std::vector<EditorItem> EditorList;

Editor* current_editor = NULL;
Widget* box_editors = NULL;

static EditorList editors;
static bool mini_editor_enabled = true; // True if the user wants to use the mini editor
static Editor* mini_editor = NULL;

static int is_document_in_some_editor(Document* document);
static Document* get_more_reliable_document();
static Widget* find_next_editor(Widget* widget);
static int count_parents(Widget* widget);

static void create_mini_editor_window();
static void hide_mini_editor_window();
static void update_mini_editor_window(Editor* editor);

class WrappedEditor : public Editor,
                      public EditorListener,
                      public EditorCustomizationDelegate
{
public:
  WrappedEditor() {
    addListener(this);
    setCustomizationDelegate(this);
  }

  ~WrappedEditor() {
    removeListener(this);
    setCustomizationDelegate(NULL);
  }

  // EditorListener implementation
  void dispose() OVERRIDE {
    // Do nothing
  }

  void scrollChanged(Editor* editor) OVERRIDE {
    update_mini_editor_window(editor);
  }

  void documentChanged(Editor* editor) OVERRIDE {
    if (editor == current_editor)
      update_mini_editor_window(editor);
  }

  void stateChanged(Editor* editor) OVERRIDE {
    // Do nothing
  }

  // EditorCustomizationDelegate implementation
  tools::Tool* getQuickTool(tools::Tool* currentTool) OVERRIDE {
    return get_selected_quicktool(currentTool);
  }

  bool isCopySelectionKeyPressed() OVERRIDE {
    Accelerator* accel = get_accel_to_copy_selection();
    if (accel)
      return accel->checkFromAllegroKeyArray();
    else
      return false;
  }

  bool isSnapToGridKeyPressed() OVERRIDE {
    Accelerator* accel = get_accel_to_snap_to_grid();
    if (accel)
      return accel->checkFromAllegroKeyArray();
    else
      return false;
  }

  bool isAngleSnapKeyPressed() OVERRIDE {
    Accelerator* accel = get_accel_to_angle_snap();
    if (accel)
      return accel->checkFromAllegroKeyArray();
    else
      return false;
  }

  bool isMaintainAspectRatioKeyPressed() OVERRIDE {
    Accelerator* accel = get_accel_to_maintain_aspect_ratio();
    if (accel)
      return accel->checkFromAllegroKeyArray();
    else
      return false;
  }

  bool isLockAxisKeyPressed() OVERRIDE {
    Accelerator* accel = get_accel_to_lock_axis();
    if (accel)
      return accel->checkFromAllegroKeyArray();
    else
      return false;
  }

};

class MiniEditor : public Editor
{
public:
  MiniEditor() {
  }

  bool changePreferredSettings() OVERRIDE {
    return false;
  }
};

class MiniEditorWindow : public Window
{
public:
  // Create mini-editor
  MiniEditorWindow() : Window(false, "Mini-Editor") {
    child_spacing = 0;
    set_autoremap(false);
    set_wantfocus(false);
  }

protected:
  void onClose(CloseEvent& ev) OVERRIDE {
    Button* closeButton = dynamic_cast<Button*>(ev.getSource());
    if (closeButton != NULL &&
        closeButton->getId() == SkinTheme::kThemeCloseButtonId) {
      // Here we don't use "enable_mini_editor" to change the state of
      // "mini_editor_enabled" because we're coming from a close event
      // of the window.
      mini_editor_enabled = false;

      // Redraw the tool bar because it shows the mini editor enabled state.
      // TODO abstract this event
      ToolBar::instance()->invalidate();
    }
  }
};

static MiniEditorWindow* mini_editor_window = NULL;

int init_module_editors()
{
  mini_editor_enabled = get_config_bool("MiniEditor", "Enabled", true);
  return 0;
}

void exit_module_editors()
{
  set_config_bool("MiniEditor", "Enabled", mini_editor_enabled);

  if (mini_editor_window) {
    save_window_pos(mini_editor_window, "MiniEditor");

    delete mini_editor_window;
    mini_editor_window = NULL;
  }

  ASSERT(editors.empty());
}

Editor* create_new_editor()
{
  Editor* editor = new WrappedEditor();
  editors.push_back(EditorItem(editor, EditorItem::Normal));
  return editor;
}

// Removes the specified editor from the "editors" list.
// It does not delete the editor.
void remove_editor(Editor* editor)
{
  for (EditorList::iterator
         it = editors.begin(),
         end = editors.end(); it != end; ++it) {
    if (it->getEditor() == editor) {
      editors.erase(it);
      return;
    }
  }

  ASSERT(false && "Editor not found in the list");
}

void refresh_all_editors()
{
  for (EditorList::iterator it = editors.begin(); it != editors.end(); ++it) {
    it->getEditor()->invalidate();
  }
}

void update_editors_with_document(const Document* document)
{
  for (EditorList::iterator it = editors.begin(); it != editors.end(); ++it) {
    Editor* editor = it->getEditor();

    if (document == editor->getDocument())
      editor->updateEditor();
  }
}

void editors_draw_sprite(const Sprite* sprite, int x1, int y1, int x2, int y2)
{
  for (EditorList::iterator it = editors.begin(); it != editors.end(); ++it) {
    Editor* editor = it->getEditor();

    if (sprite == editor->getSprite() && editor->isVisible())
      editor->drawSpriteSafe(x1, y1, x2, y2);
  }
}

// TODO improve this (with JRegion or something, and without recursivity).
void editors_draw_sprite_tiled(const Sprite* sprite, int x1, int y1, int x2, int y2)
{
  int cx1, cy1, cx2, cy2;       // Cel rectangle.
  int lx1, ly1, lx2, ly2;       // Limited rectangle to the cel rectangle.
#ifdef TILED_IN_LAYER
  Image *image = GetImage2(sprite, &cx1, &cy1, NULL);
  cx2 = cx1+image->w-1;
  cy2 = cy1+image->h-1;
#else
  cx1 = 0;
  cy1 = 0;
  cx2 = cx1+sprite->getWidth()-1;
  cy2 = cy1+sprite->getHeight()-1;
#endif

  lx1 = MAX(x1, cx1);
  ly1 = MAX(y1, cy1);
  lx2 = MIN(x2, cx2);
  ly2 = MIN(y2, cy2);

  // Draw the rectangles inside the editor.
  editors_draw_sprite(sprite, lx1, ly1, lx2, ly2);

  // Left.
  if (x1 < cx1 && lx2 < cx2) {
    editors_draw_sprite_tiled(sprite,
                              MAX(lx2+1, cx2+1+(x1-cx1)), y1,
                              cx2, y2);
  }

  // Top.
  if (y1 < cy1 && ly2 < cy2) {
    editors_draw_sprite_tiled(sprite,
                              x1, MAX(ly2+1, cy2+1+(y1-cx1)),
                              x2, cy2);
  }

  // Right.
  if (x2 >= cx2+1 && lx1 > cx1) {
#ifdef TILED_IN_LAYER
    editors_draw_sprite_tiled(sprite,
                              cx1, y1,
                              MIN(lx1-1, x2-image->w), y2);
#else
    editors_draw_sprite_tiled(sprite,
                              cx1, y1,
                              MIN(lx1-1, x2-sprite->getWidth()), y2);
#endif
  }

  // Bottom.
  if (y2 >= cy2+1 && ly1 > cy1) {
#if TILED_IN_LAYER
    editors_draw_sprite_tiled(sprite,
                              x1, cy1,
                              x2, MIN(ly1-1, y2-image->h));
#else
    editors_draw_sprite_tiled(sprite,
                              x1, cy1,
                              x2, MIN(ly1-1, y2-sprite->getHeight()));
#endif
  }
}

void editors_hide_document(const Document* document)
{
  UIContext* context = UIContext::instance();
  Document* activeDocument = context->getActiveDocument();
  Sprite* activeSprite = (activeDocument ? activeDocument->getSprite(): NULL);
  bool refresh = (activeSprite == document->getSprite()) ? true: false;

  for (EditorList::iterator it = editors.begin(); it != editors.end(); ++it) {
    Editor* editor = it->getEditor();

    if (document == editor->getDocument())
      editor->setDocument(get_more_reliable_document());
  }

  if (refresh) {
    Document* document = current_editor->getDocument();

    context->setActiveDocument(document);
    app_refresh_screen(document);
  }
}

void set_current_editor(Editor* editor)
{
  if (current_editor != editor) {
    // Here we check if the specified editor in the parameter is the
    // mini-editor, in this case, we cannot put the mini-editor as the
    // current one.
    for (EditorList::iterator it = editors.begin(); it != editors.end(); ++it) {
      if (it->getEditor() == editor) {
        if (it->getType() != EditorItem::Normal) {
          // Avoid setting the mini-editor as the current one
          return;
        }
        break;
      }
    }

    if (current_editor)
      View::getView(current_editor)->invalidate();

    current_editor = editor;

    View::getView(current_editor)->invalidate();

    UIContext* context = UIContext::instance();
    Document* document = current_editor->getDocument();
    context->setActiveDocument(document);

    app_refresh_screen(document);
    app_rebuild_documents_tabs();

    update_mini_editor_window(editor);
  }
}

void set_document_in_current_editor(Document* document)
{
  if (current_editor) {
    UIContext* context = UIContext::instance();

    context->setActiveDocument(document);
    if (document != NULL)
      context->sendDocumentToTop(document);

    current_editor->setDocument(document);

    View::getView(current_editor)->invalidate();

    app_refresh_screen(document);
    app_rebuild_documents_tabs();
  }
}

void set_document_in_more_reliable_editor(Document* document)
{
  // The current editor
  Editor* best = current_editor;

  // Search for any empty editor
  if (best->getDocument()) {
    for (EditorList::iterator it = editors.begin(); it != editors.end(); ++it) {
      // Avoid using abnormal editors (mini, etc.)
      if (it->getType() != EditorItem::Normal)
        continue;

      Editor* editor = it->getEditor();

      if (!editor->getDocument()) {
        best = editor;
        break;
      }
    }
  }

  set_current_editor(best);
  set_document_in_current_editor(document);
}

void split_editor(Editor* editor, int align)
{
  if (count_parents(editor) > 10) {
    Alert::show("Error<<You cannot split this editor more||&Close");
    return;
  }

  View* view = View::getView(editor);
  Widget* parent_box = view->getParent(); // Box or splitter.

  // Create a new box to contain both editors, and a new view to put the new editor.
  Widget* new_splitter = new Splitter(align);
  View* new_view = new EditorView(EditorView::CurrentEditorMode);
  Editor* new_editor = create_new_editor();

  // Insert the "new_box" in the same location that the view.
  parent_box->replaceChild(view, new_splitter);

  // Append the new editor.
  new_view->attachToView(new_editor);

  // Set the sprite for the new editor.
  new_editor->setDocument(editor->getDocument());
  new_editor->setZoom(editor->getZoom());

  // Expansive widgets.
  new_splitter->setExpansive(true);
  new_view->setExpansive(true);

  // Append both views to the "new_splitter".
  new_splitter->addChild(view);
  new_splitter->addChild(new_view);

  // Same position.
  {
    new_view->setViewScroll(view->getViewScroll());

    jrect_copy(new_view->rc, view->rc);
    jrect_copy(new_view->getViewport()->rc, view->getViewport()->rc);
    jrect_copy(new_editor->rc, editor->rc);

    new_editor->setOffsetX(editor->getOffsetX());
    new_editor->setOffsetY(editor->getOffsetY());
  }

  // Fixup window.
  FIXUP_TOP_WINDOW();

  // Update both editors.
  editor->updateEditor();
  new_editor->updateEditor();
}

void close_editor(Editor* editor)
{
  View* view = View::getView(editor);
  Widget* parent_box = view->getParent(); // Box or panel
  Widget* other_widget;

  // You can't remove all (normal) editors.
  int normalEditors = 0;
  for (EditorList::iterator it = editors.begin(); it != editors.end(); ++it) {
    if (it->getType() == EditorItem::Normal)
      normalEditors++;
  }
  if (normalEditors == 1) // In this case we avoid to remove the last normal editor
    return;

  // Deselect the editor.
  if (editor == current_editor)
    current_editor = 0;

  // Remove this editor.
  parent_box->removeChild(view);
  delete view;

  // Fixup the parent.
  other_widget = UI_FIRST_WIDGET(parent_box->getChildren());

  parent_box->removeChild(other_widget);
  parent_box->getParent()->replaceChild(parent_box, other_widget);
  delete parent_box;

  // Find next editor to select.
  if (!current_editor) {
    Widget* next_editor = find_next_editor(other_widget);
    if (next_editor) {
      ASSERT(next_editor->type == editor_type());

      set_current_editor(static_cast<Editor*>(next_editor));
    }
  }

  // Fixup window.
  FIXUP_TOP_WINDOW();

  // Update all editors.
  for (EditorList::iterator it = editors.begin(); it != editors.end(); ++it) {
    Editor* editor = it->getEditor();
    editor->updateEditor();
  }
}

void make_unique_editor(Editor* editor)
{
  View* view = View::getView(editor);

  // It's the unique editor.
  if (editors.size() == 1)
    return;

  // Remove the editor-view of its parent.
  view->getParent()->removeChild(view);

  // Remove all children of main_editor_box.
  while (!box_editors->getChildren().empty()) {
    Widget* child = box_editors->getChildren().front();
    box_editors->removeChild(child);
    delete child; // widget
  }

  // Append the editor to main box.
  box_editors->addChild(view);

  // New current editor.
  set_current_editor(editor);

  // Fixup window.
  FIXUP_TOP_WINDOW();

  // Update new editor.
  editor->updateEditor();
}

bool is_mini_editor_enabled()
{
  return mini_editor_enabled;
}

void enable_mini_editor(bool state)
{
  mini_editor_enabled = state;

  update_mini_editor_window(current_editor);
}

static int is_document_in_some_editor(Document* document)
{
  for (EditorList::iterator it = editors.begin(); it != editors.end(); ++it) {
    Editor* editor = it->getEditor();

    if (document == editor->getDocument())
      return true;
  }
  return false;
}

// Returns the next sprite that should be show if we close the current one.
static Document* get_more_reliable_document()
{
  UIContext* context = UIContext::instance();
  const Documents& docs = context->getDocuments();

  for (Documents::const_iterator
         it = docs.begin(), end = docs.end(); it != end; ++it) {
    Document* document = *it;
    if (!(is_document_in_some_editor(document)))
      return document;
  }

  return NULL;
}

static Widget* find_next_editor(Widget* widget)
{
  Widget* editor = NULL;

  if (widget->type == JI_VIEW) {
    editor = UI_FIRST_WIDGET(static_cast<View*>(widget)->getViewport()->getChildren());
  }
  else {
    UI_FOREACH_WIDGET(widget->getChildren(), it)
      if ((editor = find_next_editor(*it)))
        break;
  }

  return editor;
}

static int count_parents(Widget* widget)
{
  int count = 0;
  while ((widget = widget->getParent()))
    count++;
  return count;
}

static void create_mini_editor_window()
{
  // Create mini-editor
  mini_editor_window = new MiniEditorWindow();

  // Create the new for the mini editor
  View* newView = new EditorView(EditorView::AlwaysSelected);
  newView->setExpansive(true);

  // Create mini editor
  mini_editor = new MiniEditor();
  editors.push_back(EditorItem(mini_editor, EditorItem::Mini));

  newView->attachToView(mini_editor);

  mini_editor_window->addChild(newView);

  // Default bounds
  int width = JI_SCREEN_W/4;
  int height = JI_SCREEN_H/4;
  mini_editor_window->setBounds
    (gfx::Rect(JI_SCREEN_W - width - jrect_w(ToolBar::instance()->rc),
               JI_SCREEN_H - height - jrect_h(StatusBar::instance()->rc),
               width, height));

  load_window_pos(mini_editor_window, "MiniEditor");
}

static void hide_mini_editor_window()
{
  if (mini_editor_window &&
      mini_editor_window->isVisible()) {
    mini_editor_window->closeWindow(NULL);
  }
}

static void update_mini_editor_window(Editor* editor)
{
  if (!mini_editor_enabled || !editor) {
    hide_mini_editor_window();
    return;
  }

  Document* document = editor->getDocument();

  // Show the mini editor if it wasn't created yet and the user
  // zoomed in, or if the mini-editor was created and the zoom of
  // both editors is not the same.
  if (document && document->getSprite() &&
      ((!mini_editor && editor->getZoom() > 0) ||
       (mini_editor && mini_editor->getZoom() != editor->getZoom()))) {
    // If the mini window does not exist, create it
    if (!mini_editor_window)
      create_mini_editor_window();

    if (!mini_editor_window->isVisible())
      mini_editor_window->openWindow();

    gfx::Rect visibleBounds = editor->getVisibleSpriteBounds();
    gfx::Point pt = visibleBounds.getCenter();

    // Set the same location as in the given editor.
    if (mini_editor->getDocument() != document) {
      mini_editor->setDocument(document);
      mini_editor->setZoom(0);
      mini_editor->setState(EditorStatePtr(new EditorState));
    }

    mini_editor->centerInSpritePoint(pt.x, pt.y);
  }
  else {
    hide_mini_editor_window();
  }
}
