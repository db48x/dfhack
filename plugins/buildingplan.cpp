// Auto Material Select

#include <map>
#include <string>
#include <vector>
#include <algorithm>

#include "Core.h"
#include <Console.h>
#include <Export.h>
#include <PluginManager.h>
#include <VTableInterpose.h>


// DF data structure definition headers
#include "DataDefs.h"
#include "MiscUtils.h"
#include "Types.h"
#include "df/build_req_choice_genst.h"
#include "df/build_req_choice_specst.h"
#include "df/item.h"
#include "df/ui.h"
#include "df/ui_build_selector.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/items_other_id.h"
#include "df/job.h"
#include "df/world.h"
#include "df/building_constructionst.h"
#include "df/enabler.h"
#include "df/building_design.h"

#include "modules/Gui.h"
#include "modules/Screen.h"
#include "modules/Buildings.h"
#include "modules/Maps.h"
#include "modules/Items.h"

#include "TileTypes.h"
#include "df/job_item.h"
#include "df/dfhack_material_category.h"
#include "df/general_ref_building_holderst.h"
#include "modules/Job.h"
#include "df/building_design.h"
#include "df/buildings_other_id.h"
#include "modules/World.h"

using std::map;
using std::string;
using std::vector;

using namespace DFHack;
using namespace df::enums;

using df::global::gps;
using df::global::ui;
using df::global::ui_build_selector;
using df::global::world;
using df::global::enabler;

DFHACK_PLUGIN("buildingplan");
#define PLUGIN_VERSION 0.2

struct MaterialDescriptor
{
    df::item_type item_type;
    int16_t item_subtype;
    int16_t type;
    int32_t index;
    bool valid;

    bool matches(const MaterialDescriptor &a) const
    {
        return a.valid && valid && 
            a.type == type && 
            a.index == index &&
            a.item_type == item_type &&
            a.item_subtype == item_subtype;
    }
};

static command_result automaterial_cmd(color_ostream &out, vector <string> & parameters)
{
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown ( color_ostream &out )
{
    return CR_OK;
}

void OutputString(int8_t color, int &x, int &y, const std::string &text, bool newline = false, int left_margin = 0)
{
    Screen::paintString(Screen::Pen(' ', color, 0), x, y, text);
    if (newline)
    {
        ++y;
        x = left_margin;
    }
    else
        x += text.length();
}

void OutputHotkeyString(int &x, int &y, const char *text, const char *hotkey, bool newline = false, int left_margin = 0, int8_t color = COLOR_WHITE)
{
    OutputString(10, x, y, hotkey);
    string display(": ");
    display.append(text);
    OutputString(color, x, y, display, newline, left_margin);
}

void OutputToggleString(int &x, int &y, const char *text, const char *hotkey, bool state, bool newline = true, int left_margin = 0, int8_t color = COLOR_WHITE)
{
    OutputHotkeyString(x, y, text, hotkey);
    OutputString(COLOR_WHITE, x, y, ": ");
    if (state)
        OutputString(COLOR_GREEN, x, y, "Enabled", newline, left_margin);
    else
        OutputString(COLOR_GREY, x, y, "Disabled", newline, left_margin);
}

struct coord32_t
{
    int32_t x, y, z;

    df::coord get_coord16() const
    {
        return df::coord(x, y, z);
    }
};

typedef int8_t UIColor;

const int ascii_to_enum_offset = interface_key::STRING_A048 - '0';

inline string int_to_string(const int n)
{
    return static_cast<ostringstream*>( &(ostringstream() << n) )->str();
}

static void set_to_limit(int &value, const int maximum, const int min = 0)
{
    if (value < min)
        value = min;
    else if (value > maximum)
        value = maximum;
}

inline void paint_text(const UIColor color, const int &x, const int &y, const std::string &text, const UIColor background = 0)
{
    Screen::paintString(Screen::Pen(' ', color, background), x, y, text);
}

static string pad_string(string text, const int size, const bool front = true, const bool trim = false)
{
    if (text.length() > size)
    {
        if (trim && size > 10)
        {
            text = text.substr(0, size-3);
            text.append("...");
        }
        return text;
    }

    string aligned(size - text.length(), ' ');
    if (front)
    {
        aligned.append(text);
        return aligned;
    }
    else
    {
        text.append(aligned);
        return text;
    }
}


#define MAX_MASK 10
#define MAX_MATERIAL 21

#define SIDEBAR_WIDTH 30
#define COLOR_TITLE COLOR_BLUE
#define COLOR_UNSELECTED COLOR_GREY
#define COLOR_SELECTED COLOR_WHITE
#define COLOR_HIGHLIGHTED COLOR_GREEN


/*
 * List classes
 */
template <typename T>
class ListEntry
{
public:
    T elem;
    string text;
    bool selected;

    ListEntry(string text, T elem)
    {
        this->text = text;
        this->elem = elem;
        selected = false;
    }
};

template <typename T>
class ListColumn
{
public:
    string title;
    int highlighted_index;
    int display_start_offset;
    int32_t bottom_margin, search_margin, left_margin;
    bool search_entry_mode;
    bool multiselect;
    bool allow_null;
    bool auto_select;
    bool force_sort;

    ListColumn()
    {
        clear();
        left_margin = 2;
        bottom_margin = 3;
        search_margin = 38;
        highlighted_index = 0;
        multiselect = false;
        allow_null = true;
        auto_select = false;
        search_entry_mode = false;
        force_sort = false;
    }

    void clear()
    {
        list.clear();
        display_list.clear();
        display_start_offset = 0;
        max_item_width = 0;
        resize();
    }

    void resize()
    {
        display_max_rows = gps->dimy - 4 - bottom_margin;
    }

    void add(ListEntry<T> &entry)
    {
        list.push_back(entry);
        if (entry.text.length() > max_item_width)
            max_item_width = entry.text.length();
    }

    void add(const string &text, T &elem)
    {
        list.push_back(ListEntry<T>(text, elem));
        if (text.length() > max_item_width)
            max_item_width = text.length();
    }

    virtual void display_extras(const T &elem, int32_t &x, int32_t &y) const {}

    void display(const bool is_selected_column) const
    {
        int32_t y = 2;
        paint_text(COLOR_TITLE, left_margin, y, title);

        int last_index_able_to_display = display_start_offset + display_max_rows;
        for (int i = display_start_offset; i < display_list.size() && i < last_index_able_to_display; i++)
        {
            ++y;
            UIColor fg_color = (display_list[i]->selected) ? COLOR_SELECTED : COLOR_UNSELECTED;
            UIColor bg_color = (is_selected_column && i == highlighted_index) ? COLOR_HIGHLIGHTED : COLOR_BLACK;
            paint_text(fg_color, left_margin, y, display_list[i]->text, bg_color);
            int x = left_margin + display_list[i]->text.length() + 1;
            display_extras(display_list[i]->elem, x, y);
        }

        if (is_selected_column)
        {
            y = gps->dimy - bottom_margin;
            int32_t x = search_margin;
            OutputHotkeyString(x, y, "Search" ,"S");
            if (!search_string.empty() || search_entry_mode)
            {
                OutputString(COLOR_WHITE, x, y, ": ");
                OutputString(COLOR_WHITE, x, y, search_string);
                if (search_entry_mode)
                    OutputString(COLOR_LIGHTGREEN, x, y, "_");
            }
        }
    }

    void filter_display()
    {
        ListEntry<T> *prev_selected = (getDisplayListSize() > 0) ? display_list[highlighted_index] : NULL;
        display_list.clear();
        for (size_t i = 0; i < list.size(); i++)
        {
            if (search_string.empty() || list[i].text.find(search_string) != string::npos)
            {
                ListEntry<T> *entry = &list[i];
                display_list.push_back(entry);
                if (entry == prev_selected)
                    highlighted_index = display_list.size() - 1;
            }
        }
        changeHighlight(0);
    }

    void selectDefaultEntry()
    {
        for (size_t i = 0; i < display_list.size(); i++)
        {
            if (display_list[i]->selected)
            {
                highlighted_index = i;
                break;
            }
        }
    }

    void validateHighlight()
    {
        set_to_limit(highlighted_index, display_list.size() - 1);

        if (highlighted_index < display_start_offset)
            display_start_offset = highlighted_index;
        else if (highlighted_index >= display_start_offset + display_max_rows)
            display_start_offset = highlighted_index - display_max_rows + 1;

        if (auto_select || (!allow_null && list.size() == 1))
            display_list[highlighted_index]->selected = true;
    }

    void changeHighlight(const int highlight_change, const int offset_shift = 0)
    {
        if (!initHighlightChange())
            return;

        highlighted_index += highlight_change + offset_shift * display_max_rows;

        display_start_offset += offset_shift * display_max_rows;
        set_to_limit(display_start_offset, max(0, (int)(display_list.size())-display_max_rows));
        validateHighlight();
    }

    void setHighlight(const int index)
    {
        if (!initHighlightChange())
            return;

        highlighted_index = index;
        validateHighlight();
    }

    bool initHighlightChange()
    {
        if (display_list.size() == 0)
            return false;

        if (auto_select && !multiselect)
        {
            for (typename vector< ListEntry<T> >::iterator it = list.begin(); it != list.end(); it++)
            {
                it->selected = false;
            }
        }

        return true;
    }

    void toggleHighlighted()
    {
        if (auto_select)
            return;

        ListEntry<T> *entry = display_list[highlighted_index];
        if (!multiselect || !allow_null)
        {
            int selected_count = 0;
            for (size_t i = 0; i < list.size(); i++)
            {
                if (!multiselect && !entry->selected)
                    list[i].selected = false;
                if (!allow_null && list[i].selected)
                    selected_count++;
            }

            if (!allow_null && entry->selected && selected_count == 1)
                return;
        }

        entry->selected = !entry->selected;
    }

    vector<T*> getSelectedElems(bool only_one = false)
    {
        vector<T*> results;
        for (typename vector< ListEntry<T> >::iterator it = list.begin(); it != list.end(); it++)
        {
            if ((*it).selected)
            {
                results.push_back(&(*it).elem);
                if (only_one)
                    break;
            }
        }

        return results;
    }

    T* getFirstSelectedElem()
    {
        vector<T*> results = getSelectedElems(true);
        if (results.size() == 0)
            return NULL;
        else
            return results[0];
    }

    size_t getDisplayListSize()
    {
        return display_list.size();
    }

    size_t getBaseListSize()
    {
        return list.size();
    }

    bool feed(set<df::interface_key> *input)
    {
        if  (input->count(interface_key::CURSOR_UP))
        {
            search_entry_mode = false;
            changeHighlight(-1);
        }
        else if  (input->count(interface_key::CURSOR_DOWN))
        {
            search_entry_mode = false;
            changeHighlight(1);
        }
        else if  (input->count(interface_key::STANDARDSCROLL_PAGEUP))
        {
            search_entry_mode = false;
            changeHighlight(0, -1);
        }
        else if  (input->count(interface_key::STANDARDSCROLL_PAGEDOWN))
        {
            search_entry_mode = false;
            changeHighlight(0, 1);
        }
        else if (search_entry_mode)
        {
            // Search query typing mode

            df::interface_key last_token = *input->rbegin();
            if (last_token >= interface_key::STRING_A032 && last_token <= interface_key::STRING_A126)
            {
                // Standard character
                search_string += last_token - ascii_to_enum_offset;
                filter_display();
            }
            else if (last_token == interface_key::STRING_A000)
            {
                // Backspace
                if (search_string.length() > 0)
                {
                    search_string.erase(search_string.length()-1);
                    filter_display();
                }
            }
            else if (input->count(interface_key::SELECT) || input->count(interface_key::LEAVESCREEN))
            {
                // ENTER or ESC: leave typing mode
                search_entry_mode = false;
            }
            else if  (input->count(interface_key::CURSOR_LEFT) || input->count(interface_key::CURSOR_RIGHT))
            {
                // Arrow key pressed. Leave entry mode and allow screen to process key
                search_entry_mode = false;
                return false;
            }

            return true;
        }

        // Not in search query typing mode
        else if  (input->count(interface_key::SELECT) && !auto_select)
        {
            toggleHighlighted();
        }
        else if  (input->count(interface_key::CUSTOM_S))
        {
            search_entry_mode = true;
        }
        else if  (input->count(interface_key::CUSTOM_SHIFT_S))
        {
            search_string.clear();
            filter_display();
        }
        else if (enabler->tracking_on && gps->mouse_x != -1 && gps->mouse_y != -1 && enabler->mouse_lbut)
        {
            return setHighlightByMouse();
        }
        else
            return false;

        return true;
    }

    bool setHighlightByMouse()
    {
        if (gps->mouse_y >= 3 && gps->mouse_y < display_max_rows + 3 &&
            gps->mouse_x >= left_margin && gps->mouse_x < left_margin + max_item_width)
        {
            int new_index = display_start_offset + gps->mouse_y - 3;
            if (new_index < display_list.size())
                setHighlight(new_index);

            enabler->mouse_lbut = enabler->mouse_rbut = 0;

            return true;
        }

        return false;
    }

    static bool compareText(ListEntry<T> const& a, ListEntry<T> const& b)
    {
        return a.text.compare(b.text) < 0;
    }

    void doSort(bool (*function)(ListEntry<T> const&, ListEntry<T> const&))
    {
        if (force_sort || list.size() < 100)
            std::sort(list.begin(), list.end(), function);

        filter_display();
    }

    virtual void sort()
    {
        doSort(&compareText);
    }


    private:
        vector< ListEntry<T> > list;
        vector< ListEntry<T>* > display_list;
        string search_string;
        int display_max_rows;
        int max_item_width;
};


/*
 * Material Choice Screen
 */
class ViewscreenChooseMaterial : public dfhack_viewscreen
{
public:
    static bool reset_list;

    ViewscreenChooseMaterial();
    void feed(set<df::interface_key> *input);
    void render();

    std::string getFocusString() { return "buildingplan_choosemat"; }

private:
    ListColumn<df::dfhack_material_category> masks_column;
    ListColumn<MaterialInfo> materials_column;
    vector< ListEntry<df::dfhack_material_category> > all_masks;
    int selected_column;

    df::building_type btype;

    void populateMasks(const bool set_defaults = false);
    void populateMaterials(const bool set_defaults = false);

    bool addMaterialEntry(df::dfhack_material_category &selected_category, 
                            MaterialInfo &material, string name, const bool set_defaults);

    virtual void resize(int32_t x, int32_t y);

    void validateColumn();
};

bool ViewscreenChooseMaterial::reset_list = false;


ViewscreenChooseMaterial::ViewscreenChooseMaterial()
{
    selected_column = 0;
    masks_column.title = "Type";
    masks_column.multiselect = true;
    masks_column.left_margin = 2;
    materials_column.left_margin = MAX_MASK + 3;
    materials_column.title = "Material";

    masks_column.changeHighlight(0);

    vector<string> raw_masks;
    df::dfhack_material_category full_mat_mask, curr_mat_mask;
    full_mat_mask.whole = -1;
    curr_mat_mask.whole = 1;
    bitfield_to_string(&raw_masks, full_mat_mask);
    for (int i = 0; i < raw_masks.size(); i++)
    {
        if (raw_masks[i][0] == '?')
            break;

        all_masks.push_back(ListEntry<df::dfhack_material_category>(pad_string(raw_masks[i], MAX_MASK, false), curr_mat_mask));
        curr_mat_mask.whole <<= 1;
    }
    populateMasks();
    populateMaterials();

    masks_column.selectDefaultEntry();
    materials_column.selectDefaultEntry();
    materials_column.changeHighlight(0);
}

void ViewscreenChooseMaterial::populateMasks(const bool set_defaults /*= false */)
{
    masks_column.clear();
    for (vector< ListEntry<df::dfhack_material_category> >::iterator it = all_masks.begin(); it != all_masks.end(); it++)
    {
        auto entry = *it;
        if (set_defaults)
        {
            //TODO
            //if (cv->mat_mask.whole & entry.elem.whole)
                //entry.selected = true;
        }
        masks_column.add(entry);
    }
    masks_column.sort();
}
    
void ViewscreenChooseMaterial::populateMaterials(const bool set_defaults /*= false */)
{
    materials_column.clear();
    df::dfhack_material_category selected_category;
    vector<df::dfhack_material_category *> selected_materials = masks_column.getSelectedElems();
    if (selected_materials.size() == 1)
        selected_category = *selected_materials[0];
    else if (selected_materials.size() > 1)
        return;

    df::world_raws &raws = world->raws;
    for (int i = 1; i < DFHack::MaterialInfo::NUM_BUILTIN; i++)
    {
        auto obj = raws.mat_table.builtin[i];
        if (obj)
        {
            MaterialInfo material;
            material.decode(i, -1);
            addMaterialEntry(selected_category, material, material.toString(), set_defaults);
        }
    }

    for (size_t i = 0; i < raws.inorganics.size(); i++)
    {
        df::inorganic_raw *p = raws.inorganics[i];
        MaterialInfo material;
        material.decode(0, i);
        addMaterialEntry(selected_category, material, material.toString(), set_defaults);
    }

    materials_column.sort();
}

bool ViewscreenChooseMaterial::addMaterialEntry(df::dfhack_material_category &selected_category, MaterialInfo &material, 
                                                        string name, const bool set_defaults)
{
    bool selected = false;
    if (!selected_category.whole || material.matches(selected_category))
    {
        ListEntry<MaterialInfo> entry(pad_string(name, MAX_MATERIAL, false), material);
        if (set_defaults)
        {
            /*if (cv->material.matches(material))
            {
                entry.selected = true;
                selected = true;
            }*/
            //TODO
        }
        materials_column.add(entry);
    }

    return selected;
}

void ViewscreenChooseMaterial::feed(set<df::interface_key> *input)
{
    bool key_processed;
    switch (selected_column)
    {
    case 0:
        key_processed = masks_column.feed(input);
        if (input->count(interface_key::SELECT))
            populateMaterials(false);
        break;
    case 1:
        key_processed = materials_column.feed(input);
        break;
    }

    if (key_processed)
        return;

    if (input->count(interface_key::LEAVESCREEN))
    {
        input->clear();
        Screen::dismiss(this);
        return;
    }
    else if  (input->count(interface_key::SEC_SELECT))
    {
        df::dfhack_material_category mat_mask;
        vector<df::dfhack_material_category *> selected_masks = masks_column.getSelectedElems();
        for (vector<df::dfhack_material_category *>::iterator it = selected_masks.begin(); it != selected_masks.end(); it++)
        {
            mat_mask.whole |= (*it)->whole;
        }

        MaterialInfo *selected_material = materials_column.getFirstSelectedElem();
        MaterialInfo material = (selected_material) ? *selected_material : MaterialInfo();

        //TODO

        reset_list = true;
        Screen::dismiss(this);
    }
    else if  (input->count(interface_key::CURSOR_LEFT))
    {
        --selected_column;
        validateColumn();
    }
    else if  (input->count(interface_key::CURSOR_RIGHT))
    {
        selected_column++;
        validateColumn();
    }
    else if (enabler->tracking_on && enabler->mouse_lbut)
    {
        if (masks_column.setHighlightByMouse())
            selected_column = 0;
        else if (materials_column.setHighlightByMouse())
            selected_column = 1;

        enabler->mouse_lbut = enabler->mouse_rbut = 0;
    }
}

void ViewscreenChooseMaterial::render()
{
    if (Screen::isDismissed(this))
        return;

    dfhack_viewscreen::render();

    Screen::clear();
    Screen::drawBorder("  Building Material  ");

    masks_column.display(selected_column == 0);
    materials_column.display(selected_column == 1);

    int32_t y = gps->dimy - 3;
    int32_t x = 2;
    OutputHotkeyString(x, y, "Save", "Shift-Enter");
    x += 3;
    OutputHotkeyString(x, y, "Cancel", "Esc");
}

void ViewscreenChooseMaterial::validateColumn()
{
    set_to_limit(selected_column, 1);
}

void ViewscreenChooseMaterial::resize(int32_t x, int32_t y)
{
    dfhack_viewscreen::resize(x, y);
    masks_column.resize();
    materials_column.resize();
}


// START Planning 
class PlannedBuilding
{
public:
    PlannedBuilding(df::building *building) : min_quality(item_quality::Ordinary)
    {
        this->building = building;
        pos = df::coord(building->centerx, building->centery, building->z);
    }

    df::building_type getType()
    {
        return building->getType();
    }

    bool assignClosestItem(vector<df::item *> *items_vector)
    {
        decltype(items_vector->begin()) closest_item;
        int32_t closest_distance = -1;
        for (auto item_iter = items_vector->begin(); item_iter != items_vector->end(); item_iter++)
        {
            auto pos = (*item_iter)->pos;
            auto distance = abs(pos.x - building->centerx) + 
                abs(pos.y - building->centery) + 
                abs(pos.z - building->z) * 50;

            if (closest_distance > -1 && distance >= closest_distance)
                continue;

            closest_distance = distance;
            closest_item = item_iter;
        }

        if (closest_distance > -1 && assignItem(*closest_item))
        {
            items_vector->erase(closest_item);
            return true;
        }

        return false;
    }

    bool assignItem(df::item *item)
    {
        auto ref = df::allocate<df::general_ref_building_holderst>();
        if (!ref)
        {
            Core::printerr("Could not allocate general_ref_building_holderst\n");
            return false;
        }

        ref->building_id = building->id;

        auto job = building->jobs[0];
        delete job->job_items[0];
        job->job_items.clear();
        job->flags.bits.suspend = false;

        bool rough = false;
        Job::attachJobItem(job, item, df::job_item_ref::Hauled);
        if (item->getType() == item_type::BOULDER)
            rough = true;
        building->mat_type = item->getMaterial();
        building->mat_index = item->getMaterialIndex();

        job->mat_type = building->mat_type;
        job->mat_index = building->mat_index;

        if (building->needsDesign())
        {
            auto act = (df::building_actual *) building;
            act->design = new df::building_design();
            act->design->flags.bits.rough = rough;
        }

        return true;
    }

    bool isValid()
    {
        return building && Buildings::findAtTile(pos) == building;
    }

    bool isCurrentlySelectedBuilding()
    {
        return building == world->selected_building;
    }

private:
    df::building *building;
    PersistentDataItem config;
    df::coord pos;

    vector<MaterialInfo> materials;
    df::dfhack_material_category mat_mask;
    item_quality::item_quality min_quality;
};


class Planner
{
public:
    bool isPlanableBuilding(const df::building_type type) const
    {
        return item_for_building_type.find(type) != item_for_building_type.end();
    }

    void reset()
    {
        planned_buildings.clear();
        for(auto iter = world->buildings.all.begin(); iter != world->buildings.all.end(); iter++)
        {
            auto bld = *iter;
            if (isPlanableBuilding(bld->getType()))
            {
                if (bld->jobs.size() != 1)
                    continue;

                auto job = bld->jobs[0];
                if (!job->flags.bits.suspend)
                    continue;

                if (job->job_items.size() != 1)
                    continue;

                if (job->job_items[0]->item_type != item_type::NONE)
                    continue;

                addPlannedBuilding(bld);
            }
        }
    }

    void initialize()
    {
        vector<string> item_names;
        typedef df::enum_traits<df::item_type> item_types;
        int size = item_types::last_item_value - item_types::first_item_value+1;
        for (size_t i = 1; i < size; i++)
        {
            is_relevant_item_type[(df::item_type) (i-1)] = false;
            string item_name = toLower(item_types::key_table[i]);
            string item_name_clean;
            for (auto c = item_name.begin(); c != item_name.end(); c++)
            {
                if (*c == '_')
                    continue;
                item_name_clean += *c;
            }
            item_names.push_back(item_name_clean);
        }
        
        typedef df::enum_traits<df::building_type> building_types;
        size = building_types::last_item_value - building_types::first_item_value+1;
        for (size_t i = 1; i < size; i++)
        {
            auto building_type = (df::building_type) (i-1);
            if (building_type == building_type::Weapon || building_type == building_type::Floodgate)
                continue;

            string building_name = toLower(building_types::key_table[i]);
            for (size_t j = 0; j < item_names.size(); j++)
            {
                if (building_name == item_names[j])
                {
                    item_for_building_type[(df::building_type) (i-1)] = (df::item_type) j;
                    available_item_vectors[(df::item_type) j] = vector<df::item *>();
                    is_relevant_item_type[(df::item_type) j] = true;
                }
            }
        }
    }

    void addPlannedBuilding(df::building *bld)
    {
        PlannedBuilding pb(bld);
        planned_buildings.push_back(pb);
    }

    void doCycle()
    {
        if (planned_buildings.size() == 0)
            return;

        gather_available_items();
        for (auto building_iter = planned_buildings.begin(); building_iter != planned_buildings.end();)
        {
            if (building_iter->isValid())
            {
                auto required_item_type = item_for_building_type[building_iter->getType()];
                auto items_vector = &available_item_vectors[required_item_type];
                if (items_vector->size() == 0 || !building_iter->assignClosestItem(items_vector))
                {
                    ++building_iter;
                    continue;
                }
            }

            building_iter = planned_buildings.erase(building_iter);
        }
    }

    bool allocatePlannedBuilding(df::building_type type)
    {
        coord32_t cursor;
        if (!Gui::getCursorCoords(cursor.x, cursor.y, cursor.z))
            return false;

        auto newinst = Buildings::allocInstance(cursor.get_coord16(), type);
        if (!newinst)
            return false;

        df::job_item *filter = new df::job_item();
        filter->item_type = item_type::NONE;
        filter->mat_index = 0;
        filter->flags2.bits.building_material = true;
        std::vector<df::job_item*> filters;
        filters.push_back(filter);

        if (!Buildings::constructWithFilters(newinst, filters))
        {
            delete newinst;
            return false;
        }

        for (auto iter = newinst->jobs.begin(); iter != newinst->jobs.end(); iter++)
        {
            (*iter)->flags.bits.suspend = true;
        }

        addPlannedBuilding(newinst);

        return true;
    }

    bool canUnsuspendSelectedBuilding()
    {
        for (auto building_iter = planned_buildings.begin(); building_iter != planned_buildings.end(); building_iter++)
        {
            if (building_iter->isValid() && building_iter->isCurrentlySelectedBuilding())
            {
                return false;
            }
        }

        return true;
    }

private:
    map<df::building_type, df::item_type> item_for_building_type;
    map<df::item_type, vector<df::item *>> available_item_vectors;
    map<df::item_type, bool> is_relevant_item_type; //Needed for fast check when loopin over all items

    vector<PlannedBuilding> planned_buildings;

    void gather_available_items()
    {
        for (auto iter = available_item_vectors.begin(); iter != available_item_vectors.end(); iter++)
        {
            iter->second.clear();
        }

        // Precompute a bitmask with the bad flags
        df::item_flags bad_flags;
        bad_flags.whole = 0;

#define F(x) bad_flags.bits.x = true;
        F(dump); F(forbid); F(garbage_collect);
        F(hostile); F(on_fire); F(rotten); F(trader);
        F(in_building); F(construction); F(artifact1);
#undef F

        std::vector<df::item*> &items = world->items.other[items_other_id::ANY_FREE];

        for (size_t i = 0; i < items.size(); i++)
        {
            df::item *item = items[i];

            if (item->flags.whole & bad_flags.whole)
                continue;

            df::item_type itype = item->getType();
            if (!is_relevant_item_type[itype])
                continue;

            if (itype == item_type::BOX && item->isBag())
                continue; //Skip bags

            if (item->flags.bits.in_job ||
                item->isAssignedToStockpile() ||
                item->flags.bits.owned ||
                item->flags.bits.in_chest)
            {
                continue;
            }

            available_item_vectors[itype].push_back(item);
        }
    }
};

static Planner planner;


static map<df::building_type, bool> planmode_enabled;
static bool is_planmode_enabled(df::building_type type)
{
    if (planmode_enabled.find(type) == planmode_enabled.end())
    {
        planmode_enabled[type] = false;
    }

    return planmode_enabled[type];
}

#define DAY_TICKS 1200
DFhackCExport command_result plugin_onupdate(color_ostream &out)
{
    static decltype(world->frame_counter) last_frame_count = 0;
    if ((world->frame_counter - last_frame_count) >= DAY_TICKS/2)
    {
        last_frame_count = world->frame_counter;
        planner.doCycle();
    }

    return CR_OK;
}

//START Viewscreen Hook
struct buildingplan_hook : public df::viewscreen_dwarfmodest
{
    //START UI Methods
    typedef df::viewscreen_dwarfmodest interpose_base;

    void send_key(const df::interface_key &key)
    {
        set< df::interface_key > keys;
        keys.insert(key);
        this->feed(&keys);
    }

    bool isInPlannedBuildingQueryMode()
    {
        return (ui->main.mode == df::ui_sidebar_mode::QueryBuilding || 
            ui->main.mode == df::ui_sidebar_mode::BuildingItems) &&
            !planner.canUnsuspendSelectedBuilding();
    }

    bool isInPlannedBuildingPlacementMode()
    {
        return ui->main.mode == ui_sidebar_mode::Build &&
            ui_build_selector &&
            ui_build_selector->stage < 2 &&
            planner.isPlanableBuilding(ui_build_selector->building_type);
    }

    bool handle_input(set<df::interface_key> *input)
    {
        if (isInPlannedBuildingPlacementMode())
        {
            auto type = ui_build_selector->building_type;
            if (input->count(interface_key::CUSTOM_P))
            {
                planmode_enabled[type] = !planmode_enabled[type];
                if (!planmode_enabled[type])
                {
                    send_key(interface_key::CURSOR_DOWN_Z);
                    send_key(interface_key::CURSOR_UP_Z);
                }
                return true;
            }
            
            if (is_planmode_enabled(type))
            {
                if (input->count(interface_key::SELECT))
                {
                    if (ui_build_selector->errors.size() == 0 && planner.allocatePlannedBuilding(type))
                    {
                        send_key(interface_key::CURSOR_DOWN_Z);
                        send_key(interface_key::CURSOR_UP_Z);
                    }

                    return true;
                }
            }
        }
        else if (isInPlannedBuildingQueryMode() &&
            input->count(interface_key::SUSPENDBUILDING))
        {
            return true; // Don't unsuspend planned buildings
        }

        return false;
    }

    DEFINE_VMETHOD_INTERPOSE(void, feed, (set<df::interface_key> *input))
    {
        if (!handle_input(input))
            INTERPOSE_NEXT(feed)(input);
    }

    DEFINE_VMETHOD_INTERPOSE(void, render, ())
    {
        bool plannable = isInPlannedBuildingPlacementMode();
        if (plannable && is_planmode_enabled(ui_build_selector->building_type))
        {
            if (ui_build_selector->stage < 1)
            {
                // No materials but turn on cursor
                ui_build_selector->stage = 1;
            }

            for (auto iter = ui_build_selector->errors.begin(); iter != ui_build_selector->errors.end();)
            {
                //FIXME Hide bags
                if (((*iter)->find("Needs") != string::npos && **iter != "Needs adjacent wall")  ||
                    (*iter)->find("No access") != string::npos)
                {
                    iter = ui_build_selector->errors.erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
        }

        INTERPOSE_NEXT(render)();

        auto dims = Gui::getDwarfmodeViewDims();
        int left_margin = dims.menu_x1 + 1;
        int x = left_margin;
        if (plannable)
        {
            int y = 23;

            OutputToggleString(x, y, "Planning Mode", "p", is_planmode_enabled(ui_build_selector->building_type), true, left_margin);

            if (is_planmode_enabled(ui_build_selector->building_type))
            {
                //OutputHotkeyString(x, y, "Material Filter", "m", true, left_margin);
            }
        }
        else if (isInPlannedBuildingQueryMode())
        {
            // Hide suspend toggle option
            int y = 20;
            Screen::Pen pen(' ', COLOR_BLACK);
            Screen::fillRect(pen, x, y, dims.menu_x2, y);
        }
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(buildingplan_hook, feed);
IMPLEMENT_VMETHOD_INTERPOSE(buildingplan_hook, render);


static command_result buildingplan_cmd(color_ostream &out, vector <string> & parameters)
{
    if (!parameters.empty())
    {
        out << "Building Plan" << endl << "Version: " << PLUGIN_VERSION << endl;
    }

    return CR_OK;
}


DFhackCExport command_result plugin_init ( color_ostream &out, std::vector <PluginCommand> &commands)
{
    if (!gps || !INTERPOSE_HOOK(buildingplan_hook, feed).apply() || !INTERPOSE_HOOK(buildingplan_hook, render).apply())
        out.printerr("Could not insert buildingplan hooks!\n");

    commands.push_back(
        PluginCommand(
        "buildingplan", "Place furniture before it's built",
        buildingplan_cmd, false, ""));

    planner.initialize();

    return CR_OK;
}


DFhackCExport command_result plugin_onstatechange(color_ostream &out, state_change_event event)
{
    switch (event) {
    case SC_MAP_LOADED:
        planner.reset();
        break;
    default:
        break;
    }

    return CR_OK;
}