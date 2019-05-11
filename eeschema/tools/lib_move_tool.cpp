/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2019 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <tool/tool_manager.h>
#include <tools/ee_selection_tool.h>
#include <ee_actions.h>
#include <ee_hotkeys.h>
#include <view/view.h>
#include <bitmaps.h>
#include <base_struct.h>
#include <lib_edit_frame.h>
#include <eeschema_id.h>
#include "lib_move_tool.h"
#include "lib_pin_tool.h"


LIB_MOVE_TOOL::LIB_MOVE_TOOL() :
        TOOL_INTERACTIVE( "libedit.InteractiveMove" ),
        m_selectionTool( nullptr ),
        m_controls( nullptr ),
        m_frame( nullptr ),
        m_menu( *this ),
        m_moveInProgress( false ),
        m_moveOffset( 0, 0 )
{
}


LIB_MOVE_TOOL::~LIB_MOVE_TOOL()
{
}


bool LIB_MOVE_TOOL::Init()
{
    m_frame = getEditFrame<LIB_EDIT_FRAME>();
    m_selectionTool = m_toolMgr->GetTool<EE_SELECTION_TOOL>();

    wxASSERT_MSG( m_selectionTool, "eeshema.InteractiveSelection tool is not available" );

    //
    // Build the tool menu
    //
    CONDITIONAL_MENU& ctxMenu = m_menu.GetMenu();

    ctxMenu.AddItem( ACTIONS::cancelInteractive, EE_CONDITIONS::ShowAlways, 1 );

    ctxMenu.AddSeparator( EE_CONDITIONS::ShowAlways, 1000 );
    m_menu.AddStandardSubMenus( m_frame );

    //
    // Add move actions to the selection tool menu
    //
    CONDITIONAL_MENU& selToolMenu = m_selectionTool->GetToolMenu().GetMenu();

    selToolMenu.AddItem( EE_ACTIONS::move, EE_CONDITIONS::IdleSelection, 150 );

    return true;
}


void LIB_MOVE_TOOL::Reset( RESET_REASON aReason )
{
    if( aReason == MODEL_RELOAD )
    {
        m_moveInProgress = false;
        m_moveOffset = { 0, 0 };

        // Init variables used by every drawing tool
        m_controls = getViewControls();
        m_frame = getEditFrame<LIB_EDIT_FRAME>();
    }
}


int LIB_MOVE_TOOL::Main( const TOOL_EVENT& aEvent )
{
    KIGFX::VIEW_CONTROLS* controls = getViewControls();

    controls->SetSnapping( true );
    VECTOR2I originalCursorPos = controls->GetCursorPosition();

    // Be sure that there is at least one item that we can move. If there's no selection try
    // looking for the stuff under mouse cursor (i.e. Kicad old-style hover selection).
    SELECTION& selection = m_selectionTool->RequestSelection();
    bool       unselect = selection.IsHover();

    if( selection.Empty() )
        return 0;

    m_frame->SetToolID( ID_SCH_MOVE, wxCURSOR_DEFAULT, _( "Move Items" ) );

    Activate();
    controls->ShowCursor( true );
    controls->SetAutoPan( true );

    bool restore_state = false;
    bool chain_commands = false;
    OPT_TOOL_EVENT evt = aEvent;
    VECTOR2I prevPos;

    if( !selection.Front()->IsNew() )
        m_frame->SaveCopyInUndoList( m_frame->GetCurPart() );

    // Main loop: keep receiving events
    do
    {
        controls->SetSnapping( !evt->Modifier( MD_ALT ) );

        if( evt->IsAction( &EE_ACTIONS::move ) || evt->IsMotion() || evt->IsDrag( BUT_LEFT )
                || evt->IsAction( &EE_ACTIONS::refreshPreview ) )
        {
            if( !m_moveInProgress )    // Prepare to start moving/dragging
            {
                // Pick up any synchronized pins
                //
                if( selection.GetSize() == 1 && selection.Front()->Type() == LIB_PIN_T
                        && m_frame->SynchronizePins() )
                {
                    LIB_PIN*  cur_pin = (LIB_PIN*) selection.Front();
                    LIB_PART* part = m_frame->GetCurPart();

                    for( LIB_PIN* pin = part->GetNextPin(); pin; pin = part->GetNextPin( pin ) )
                    {
                        if( pin->GetPosition() == cur_pin->GetPosition()
                         && pin->GetOrientation() == cur_pin->GetOrientation()
                         && pin->GetConvert() == cur_pin->GetConvert() )
                        {
                            m_selectionTool->AddItemToSel( pin, true );
                        }
                    }
                }

                // Apply any initial offset in case we're coming from a previous command.
                //
                for( EDA_ITEM* item : selection )
                    moveItem( item, m_moveOffset );

                // Set up the starting position and move/drag offset
                //
                m_cursor = controls->GetCursorPosition();

                if( selection.HasReferencePoint() )
                {
                    VECTOR2I delta = m_cursor - selection.GetReferencePoint();

                    // Drag items to the current cursor position
                    for( EDA_ITEM* item : selection )
                    {
                        // Don't double move pins, fields, etc.
                        if( item->GetParent() && item->GetParent()->IsSelected() )
                            continue;

                        moveItem( item, delta );
                        getView()->Update( item );
                    }

                    selection.SetReferencePoint( m_cursor );
                }
                else if( selection.Size() == 1 )
                {
                    // Set the current cursor position to the first dragged item origin,
                    // so the movement vector can be computed later
                    updateModificationPoint( selection );
                    m_cursor = originalCursorPos;
                }
                else
                {
                    updateModificationPoint( selection );
                }

                controls->SetCursorPosition( m_cursor, false );

                prevPos = m_cursor;
                controls->SetAutoPan( true );
                m_moveInProgress = true;
            }

            //------------------------------------------------------------------------
            // Follow the mouse
            //
            m_cursor = controls->GetCursorPosition();
            VECTOR2I delta( m_cursor - prevPos );
            selection.SetReferencePoint( m_cursor );

            m_moveOffset += delta;
            prevPos = m_cursor;

            for( EDA_ITEM* item : selection )
            {
                moveItem( item, delta );
                getView()->Update( item );
            }

            m_toolMgr->PostEvent( EVENTS::SelectedItemsModified );
            m_frame->UpdateMsgPanel();
        }
        //------------------------------------------------------------------------
        // Handle cancel
        //
        else if( TOOL_EVT_UTILS::IsCancelInteractive( evt.get() ) )
        {
            if( m_moveInProgress )
                restore_state = true;

            break;
        }
        //------------------------------------------------------------------------
        // Handle TOOL_ACTION special cases
        //
        else if( evt->Action() == TA_UNDO_REDO_PRE )
        {
            unselect = true;
            break;
        }
        else if( evt->Category() == TC_COMMAND )
        {
            if( evt->IsAction( &EE_ACTIONS::doDelete ) )
            {
                // Exit on a remove operation; there is no further processing for removed items.
                break;
            }
            else if( evt->IsAction( &EE_ACTIONS::duplicate ) )
            {
                if( selection.Front()->IsNew() )
                {
                    // This doesn't really make sense; we'll just end up dragging a stack of
                    // objects so Duplicate() is going to ignore this and we'll just carry on.
                    continue;
                }

                // Move original back and exit.  The duplicate will run in its own loop.
                restore_state = true;
                unselect = false;
                chain_commands = true;
                break;
            }
        }
        //------------------------------------------------------------------------
        // Handle context menu
        //
        else if( evt->IsClick( BUT_RIGHT ) )
        {
            m_menu.ShowContextMenu( selection );
        }
        //------------------------------------------------------------------------
        // Handle drop
        //
        else if( evt->IsMouseUp( BUT_LEFT ) || evt->IsClick( BUT_LEFT ) )
        {
            if( selection.GetSize() == 1 && selection.Front()->Type() == LIB_PIN_T )
            {
                LIB_PIN_TOOL* pinTool = m_toolMgr->GetTool<LIB_PIN_TOOL>();

                if( !pinTool->PlacePin( (LIB_PIN*) selection.Front() ) )
                    restore_state = true;
            }

            break; // Finish
        }

    } while( ( evt = Wait() ) ); //Should be assignment not equality test

    controls->ForceCursorPosition( false );
    controls->ShowCursor( false );
    controls->SetSnapping( false );
    controls->SetAutoPan( false );

    if( !chain_commands )
        m_moveOffset = { 0, 0 };

    m_moveInProgress = false;
    m_frame->SetNoToolSelected();

    selection.ClearReferencePoint();

    for( auto item : selection )
        item->ClearEditFlags();

    if( unselect )
        m_toolMgr->RunAction( EE_ACTIONS::clearSelection, true );

    if( restore_state )
        m_frame->RollbackPartFromUndo();
    else
        m_frame->OnModify();

    return 0;
}


void LIB_MOVE_TOOL::moveItem( EDA_ITEM* aItem, VECTOR2I aDelta )
{
    static_cast<LIB_ITEM*>( aItem )->Offset( mapCoords( aDelta ));
    aItem->SetFlags( IS_MOVED );
}


bool LIB_MOVE_TOOL::updateModificationPoint( SELECTION& aSelection )
{
    if( m_moveInProgress && aSelection.HasReferencePoint() )
        return false;

    // When there is only one item selected, the reference point is its position...
    if( aSelection.Size() == 1 )
    {
        LIB_ITEM* item =  static_cast<LIB_ITEM*>( aSelection.Front() );
        aSelection.SetReferencePoint( item->GetPosition() );
    }
    // ...otherwise modify items with regard to the grid-snapped cursor position
    else
    {
        m_cursor = getViewControls()->GetCursorPosition( true );
        aSelection.SetReferencePoint( m_cursor );
    }

    return true;
}


void LIB_MOVE_TOOL::setTransitions()
{
    Go( &LIB_MOVE_TOOL::Main,               EE_ACTIONS::move.MakeEvent() );
}
