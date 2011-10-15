/*
 * $Id$
 *
 * ***** BEGIN GPL LICENSE BLOCK *****
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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Blender Foundation 2009.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/interface/interface_node.c
 *  \ingroup edinterface
 */

#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_depsgraph.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_node.h"
#include "BKE_scene.h"

#include "RNA_access.h"

#include "WM_api.h"
#include "WM_types.h"

#include "UI_interface.h"
#include "interface_intern.h"

#include "ED_node.h"

/************************* Node Link Menu **************************/

#define UI_NODE_LINK_ADD		0
#define UI_NODE_LINK_DISCONNECT	-1
#define UI_NODE_LINK_REMOVE		-2

typedef struct NodeLinkArg {
	bNodeTree *ntree;
	bNode *node;
	bNodeSocket *sock;

	bNodeTree *ngroup;
	int type;
	int output;
} NodeLinkArg;

static void ui_node_tag_recursive(bNode *node)
{
	bNodeSocket *input;

	if(!node || (node->flag & NODE_TEST))
		return; /* in case of cycles */
	
	node->flag |= NODE_TEST;

	for(input=node->inputs.first; input; input=input->next)
		if(input->link)
			ui_node_tag_recursive(input->link->fromnode);
}

static void ui_node_clear_recursive(bNode *node)
{
	bNodeSocket *input;

	if(!node || !(node->flag & NODE_TEST))
		return; /* in case of cycles */
	
	node->flag &= ~NODE_TEST;

	for(input=node->inputs.first; input; input=input->next)
		if(input->link)
			ui_node_clear_recursive(input->link->fromnode);
}

static void ui_node_remove_linked(bNodeTree *ntree, bNode *rem_node)
{
	bNode *node, *next;
	bNodeSocket *sock;

	if(!rem_node)
		return;

	/* tag linked nodes to be removed */
	for(node=ntree->nodes.first; node; node=node->next)
		node->flag &= ~NODE_TEST;
	
	ui_node_tag_recursive(rem_node);

	/* clear tags on nodes that are still used by other nodes */
	for(node=ntree->nodes.first; node; node=node->next)
		if(!(node->flag & NODE_TEST))
			for(sock=node->inputs.first; sock; sock=sock->next)
				if(sock->link && sock->link->fromnode != rem_node)
					ui_node_clear_recursive(sock->link->fromnode);

	/* remove nodes */
	for(node=ntree->nodes.first; node; node=next) {
		next = node->next;

		if(node->flag & NODE_TEST) {
			if(node->id)
				node->id->us--;
			nodeFreeNode(ntree, node);
		}
	}
	
	//node_tree_verify_groups(ntree);
}

static void ui_node_sock_name(bNodeSocket *sock, char name[UI_MAX_NAME_STR])
{
	if(sock->link && sock->link->fromnode) {
		bNode *node = sock->link->fromnode;
		char node_name[UI_MAX_NAME_STR];

		if(node->type == NODE_GROUP)
			BLI_strncpy(node_name, node->id->name+2, UI_MAX_NAME_STR);
		else
			BLI_strncpy(node_name, node->typeinfo->name, UI_MAX_NAME_STR);

		if(node->inputs.first == NULL &&
		   node->outputs.first != node->outputs.last &&
		   !(node->typeinfo->flag & NODE_OPTIONS))
			BLI_snprintf(name, UI_MAX_NAME_STR, "%s | %s", node_name, sock->link->fromsock->name);
		else
			BLI_strncpy(name, node_name, UI_MAX_NAME_STR);
	}
	else if(sock->type == SOCK_SHADER)
		BLI_strncpy(name, "None", UI_MAX_NAME_STR);
	else
		BLI_strncpy(name, "Default", UI_MAX_NAME_STR);
}

static void ui_node_link(bContext *C, void *arg_p, void *event_p)
{
	NodeLinkArg *arg = (NodeLinkArg*)arg_p;
	bNode *node_to = arg->node;
	bNodeSocket *sock_to = arg->sock;
	bNodeTree *ntree = arg->ntree;
	bNode *node_from;
	bNodeSocket *sock_from;
	int event = GET_INT_FROM_POINTER(event_p);

	if(event == UI_NODE_LINK_DISCONNECT) {
		/* disconnect */
		if(sock_to->link)
			nodeRemLink(ntree, sock_to->link);
	}
	else if(event == UI_NODE_LINK_REMOVE) {
		/* remove */
		if(sock_to->link)
			ui_node_remove_linked(ntree, sock_to->link->fromnode);
	}
	else {
		bNode *node_prev = NULL;

		/* unlink existing node */
		if(sock_to->link) {
			node_prev = sock_to->link->fromnode;
			nodeRemLink(ntree, sock_to->link);
		}

		/* find existing node that we can use */
		for(node_from=ntree->nodes.first; node_from; node_from=node_from->next)
			if(node_from->type == arg->type)
				break;

		if(node_from)
			if(!(node_from->inputs.first == NULL && !(node_from->typeinfo->flag & NODE_OPTIONS)))
				node_from = NULL;

		if(node_prev && node_prev->type == arg->type &&
			(arg->type != NODE_GROUP || node_prev->id == &arg->ngroup->id)) {
			/* keep the previous node if it's the same type */
			node_from = node_prev;
		}
		else if(!node_from) {
			bNodeTemplate ntemp;

			/* add new node */
			if(arg->ngroup) {
				ntemp.type = NODE_GROUP;
				ntemp.ngroup = arg->ngroup;
			}
			else
				ntemp.type = arg->type;

			node_from= nodeAddNode(ntree, &ntemp);
			node_from->locx = node_to->locx - (node_from->typeinfo->width + 50);
			node_from->locy = node_to->locy;

			if(node_from->id)
				id_us_plus(node_from->id);
		}

		nodeSetActive(ntree, node_from);

		/* add link */
		sock_from = BLI_findlink(&node_from->outputs, arg->output);
		nodeAddLink(ntree, node_from, sock_from, node_to, sock_to);

		/* copy input sockets from previous node */
		if(node_prev && node_from != node_prev) {
			bNodeSocket *sock_prev, *sock_from;

			for(sock_prev=node_prev->inputs.first; sock_prev; sock_prev=sock_prev->next) {
				for(sock_from=node_from->inputs.first; sock_from; sock_from=sock_from->next) {
					if(strcmp(sock_prev->name, sock_from->name) == 0 && sock_prev->type == sock_from->type) {
						bNodeLink *link = sock_prev->link;

						if(link && link->fromnode) {
							nodeAddLink(ntree, link->fromnode, link->fromsock, node_from, sock_from);
							nodeRemLink(ntree, link);
						}

						if(sock_prev->default_value) {
							if(sock_from->default_value)
								MEM_freeN(sock_from->default_value);

							sock_from->default_value = MEM_dupallocN(sock_prev->default_value);
						}
					}
				}
			}

			/* remove node */
			ui_node_remove_linked(ntree, node_prev);
		}

		NodeTagChanged(ntree, node_from);
	}

	NodeTagChanged(ntree, node_to);
	ntreeUpdateTree(ntree);

	ED_node_generic_update(CTX_data_main(C), ntree, node_to);
}

static int ui_compatible_sockets(int typeA, int typeB)
{
	if(typeA == SOCK_SHADER || typeB == SOCK_SHADER)
		return (typeA == typeB);
	
	return (typeA == typeB);
}

static void ui_node_menu_column(Main *bmain, NodeLinkArg *arg, uiLayout *layout, const char *cname, int nclass, int compatibility)
{
	bNodeTree *ntree = arg->ntree;
	bNodeSocket *sock = arg->sock;
	uiLayout *column = NULL;
	uiBlock *block = uiLayoutGetBlock(layout);
	uiBut *but;
	bNodeType *ntype;
	bNodeTree *ngroup;
	NodeLinkArg *argN;
	int first = 1;

	if(nclass == NODE_CLASS_GROUP) {
		for(ngroup=bmain->nodetree.first; ngroup; ngroup=ngroup->id.next) {
			bNodeSocket *gsock;
			char name[UI_MAX_NAME_STR];
			int i, j, num = 0;

			if(ngroup->type != ntree->type)
				continue;

			for(gsock=ngroup->inputs.first; gsock; gsock=gsock->next)
				if(ui_compatible_sockets(gsock->type, sock->type))
					num++;

			for(i=0, j=0, gsock=ngroup->outputs.first; gsock; gsock=gsock->next, i++) {
				if(!ui_compatible_sockets(gsock->type, sock->type))
					continue;

				if(first) {
					column= uiLayoutColumn(layout, 0);
					uiBlockSetCurLayout(block, column);

					uiItemL(column, cname, ICON_NONE);
					but= block->buttons.last;
					but->flag= UI_TEXT_LEFT;

					first = 0;
				}

				if(num > 1) {
					if(j == 0) {
						uiItemL(column, ngroup->id.name+2, ICON_NONE);
						but= block->buttons.last;
						but->flag= UI_TEXT_LEFT;
					}

					BLI_snprintf(name, UI_MAX_NAME_STR, "  %s", gsock->name);
					j++;
				}
				else
					BLI_strncpy(name, ngroup->id.name+2, UI_MAX_NAME_STR);

				but = uiDefBut(block, BUT, 0, ngroup->id.name+2, 0, 0, UI_UNIT_X*4, UI_UNIT_Y,
					NULL, 0.0, 0.0, 0.0, 0.0, "Add node to input");

				argN = MEM_dupallocN(arg);
				argN->ngroup = ngroup;
				argN->output = i;
				uiButSetNFunc(but, ui_node_link, argN, NULL);
			}
		}
	}
	else {
		bNodeTreeType *ttype= ntreeGetType(ntree->type);

		for(ntype=ttype->node_types.first; ntype; ntype=ntype->next) {
			bNodeSocketTemplate *stemp;
			char name[UI_MAX_NAME_STR];
			int i, j, num = 0;

			if(compatibility && !(ntype->compatibility & compatibility))
				continue;

			if(ntype->nclass != nclass)
				continue;

			for(i=0, stemp=ntype->outputs; stemp && stemp->type != -1; stemp++, i++)
				if(ui_compatible_sockets(stemp->type, sock->type))
					num++;

			for(i=0, j=0, stemp=ntype->outputs; stemp && stemp->type != -1; stemp++, i++) {
				if(!ui_compatible_sockets(stemp->type, sock->type))
					continue;

				if(first) {
					column= uiLayoutColumn(layout, 0);
					uiBlockSetCurLayout(block, column);

					uiItemL(column, cname, ICON_NONE);
					but= block->buttons.last;
					but->flag= UI_TEXT_LEFT;

					first = 0;
				}

				if(num > 1) {
					if(j == 0) {
						uiItemL(column, ntype->name, ICON_NONE);
						but= block->buttons.last;
						but->flag= UI_TEXT_LEFT;
					}

					BLI_snprintf(name, UI_MAX_NAME_STR, "  %s", stemp->name);
					j++;
				}
				else
					BLI_strncpy(name, ntype->name, UI_MAX_NAME_STR);

				but = uiDefBut(block, BUT, 0, name, 0, 0, UI_UNIT_X*4, UI_UNIT_Y,
					NULL, 0.0, 0.0, 0.0, 0.0, "Add node to input");

				argN = MEM_dupallocN(arg);
				argN->type = ntype->type;
				argN->output = i;
				uiButSetNFunc(but, ui_node_link, argN, NULL);
			}
		}
	}
}

static void ui_template_node_link_menu(bContext *C, uiLayout *layout, void *but_p)
{
	Main *bmain= CTX_data_main(C);
	Scene *scene= CTX_data_scene(C);
	uiBlock *block = uiLayoutGetBlock(layout);
	uiBut *but = (uiBut*)but_p;
	uiLayout *split, *column;
	NodeLinkArg *arg = (NodeLinkArg*)but->func_argN;
	bNodeSocket *sock = arg->sock;
	int compatibility= 0;

	if(arg->ntree->type == NTREE_SHADER) {
		if(scene_use_new_shading_nodes(scene))
			compatibility= NODE_NEW_SHADING;
		else
			compatibility= NODE_OLD_SHADING;
	}
	
	uiBlockSetCurLayout(block, layout);
	split= uiLayoutSplit(layout, 0, 0);

	ui_node_menu_column(bmain, arg, split, "Input", NODE_CLASS_INPUT, compatibility);
	ui_node_menu_column(bmain, arg, split, "Output", NODE_CLASS_OUTPUT, compatibility);
	ui_node_menu_column(bmain, arg, split, "Shader", NODE_CLASS_SHADER, compatibility);
	ui_node_menu_column(bmain, arg, split, "Texture", NODE_CLASS_TEXTURE, compatibility);
	ui_node_menu_column(bmain, arg, split, "Color", NODE_CLASS_OP_COLOR, compatibility);
	ui_node_menu_column(bmain, arg, split, "Vector", NODE_CLASS_OP_VECTOR, compatibility);
	ui_node_menu_column(bmain, arg, split, "Convertor", NODE_CLASS_CONVERTOR, compatibility);

	column= uiLayoutColumn(split, 0);
	uiBlockSetCurLayout(block, column);

	if(sock->link) {
		uiItemL(column, "Link", ICON_NONE);
		but= block->buttons.last;
		but->flag= UI_TEXT_LEFT;

		but = uiDefBut(block, BUT, 0, "Remove", 0, 0, UI_UNIT_X*4, UI_UNIT_Y,
			NULL, 0.0, 0.0, 0.0, 0.0, "Remove nodes connected to the input");
		uiButSetNFunc(but, ui_node_link, MEM_dupallocN(arg), SET_INT_IN_POINTER(UI_NODE_LINK_REMOVE));

		but = uiDefBut(block, BUT, 0, "Disconnect", 0, 0, UI_UNIT_X*4, UI_UNIT_Y,
			NULL, 0.0, 0.0, 0.0, 0.0, "Disconnect nodes connected to the input");
		uiButSetNFunc(but, ui_node_link, MEM_dupallocN(arg), SET_INT_IN_POINTER(UI_NODE_LINK_DISCONNECT));
	}

	ui_node_menu_column(bmain, arg, column, "Group", NODE_CLASS_GROUP, compatibility);
}

void uiTemplateNodeLink(uiLayout *layout, bNodeTree *ntree, bNode *node, bNodeSocket *sock)
{
	uiBlock *block = uiLayoutGetBlock(layout);
	NodeLinkArg *arg;
	uiBut *but;

	arg = MEM_callocN(sizeof(NodeLinkArg), "NodeLinkArg");
	arg->ntree = ntree;
	arg->node = node;
	arg->sock = sock;
	arg->type = 0;
	arg->output = 0;

	uiBlockSetCurLayout(block, layout);

	if(sock->link || sock->type == SOCK_SHADER || (sock->flag & SOCK_HIDE_VALUE)) {
		char name[UI_MAX_NAME_STR];
		ui_node_sock_name(sock, name);
		but= uiDefMenuBut(block, ui_template_node_link_menu, NULL, name, 0, 0, UI_UNIT_X*4, UI_UNIT_Y, "");
	}
	else
		but= uiDefIconMenuBut(block, ui_template_node_link_menu, NULL, ICON_NONE, 0, 0, UI_UNIT_X, UI_UNIT_Y, "");

	but->type= MENU;
	but->flag |= UI_TEXT_LEFT|UI_BUT_NODE_LINK;
	but->poin= (char*)but;
	but->func_argN = arg;

	if(sock->link && sock->link->fromnode)
		if(sock->link->fromnode->flag & NODE_ACTIVE_TEXTURE)
			but->flag |= UI_BUT_NODE_ACTIVE;
}

/************************* Node Tree Layout **************************/

static void ui_node_draw_input(uiLayout *layout, bContext *C,
	bNodeTree *ntree, bNode *node, bNodeSocket *input, int depth);

static void ui_node_draw_node(uiLayout *layout, bContext *C, bNodeTree *ntree, bNode *node, int depth)
{
	bNodeSocket *input;
	uiLayout *col, *split;
	PointerRNA nodeptr;

	RNA_pointer_create(&ntree->id, &RNA_Node, node, &nodeptr);

	if(node->typeinfo->uifunc) {
		if(node->type != NODE_GROUP) {
			split = uiLayoutSplit(layout, 0.35f, 0);
			col = uiLayoutColumn(split, 0);
			col = uiLayoutColumn(split, 0);

			node->typeinfo->uifunc(col, C, &nodeptr);
		}
	}

	for(input=node->inputs.first; input; input=input->next)
		ui_node_draw_input(layout, C, ntree, node, input, depth+1);
}

static void ui_node_draw_input(uiLayout *layout, bContext *C, bNodeTree *ntree, bNode *node, bNodeSocket *input, int depth)
{
	PointerRNA inputptr;
	uiBlock *block = uiLayoutGetBlock(layout);
	uiBut *bt;
	uiLayout *split, *row, *col;
	bNode *lnode;
	char label[UI_MAX_NAME_STR];
	int indent = (depth > 1)? 2*(depth - 1): 0;

	/* to avoid eternal loops on cyclic dependencies */
	node->flag |= NODE_TEST;
	lnode = (input->link)? input->link->fromnode: NULL;

	/* socket RNA pointer */
	RNA_pointer_create(&ntree->id, &RNA_NodeSocket, input, &inputptr);

	/* indented label */
	memset(label, ' ', indent);
	label[indent] = '\0';
	BLI_snprintf(label, UI_MAX_NAME_STR, "%s%s:", label, input->name);

	/* split in label and value */
	split = uiLayoutSplit(layout, 0.35f, 0);

	row = uiLayoutRow(split, 1);

	if(depth > 0) {
		uiBlockSetEmboss(block, UI_EMBOSSN);

		if(lnode && (lnode->inputs.first || (lnode->typeinfo->uifunc && lnode->type != NODE_GROUP))) {
			int icon = (input->flag & SOCK_COLLAPSED)? ICON_DISCLOSURE_TRI_RIGHT: ICON_DISCLOSURE_TRI_DOWN;
			uiItemR(row, &inputptr, "show_expanded", UI_ITEM_R_ICON_ONLY, "", icon);
		}
		else
			uiItemL(row, "", ICON_BLANK1);

		bt = block->buttons.last;
		bt->x2 = UI_UNIT_X/2;

		uiBlockSetEmboss(block, UI_EMBOSS);
	}

	uiItemL(row, label, ICON_NONE);

	if(lnode) {
		/* input linked to a node */
		uiTemplateNodeLink(split, ntree, node, input);

		if(!(input->flag & SOCK_COLLAPSED)) {
			if(depth == 0)
				uiItemS(layout);

			ui_node_draw_node(layout, C, ntree, lnode, depth);
		}
	}
	else {
		/* input not linked, show value */
		if(input->type != SOCK_SHADER && !(input->flag & SOCK_HIDE_VALUE)) {
			if(input->type == SOCK_VECTOR) {
				row = uiLayoutRow(split, 0);
				col = uiLayoutColumn(row, 0);

				uiItemR(col, &inputptr, "default_value", 0, "", 0);
			}
			else {
				row = uiLayoutRow(split, 1);
				uiItemR(row, &inputptr, "default_value", 0, "", 0);
			}
		}
		else
			row = uiLayoutRow(split, 0);

		uiTemplateNodeLink(row, ntree, node, input);
	}

	/* clear */
	node->flag &= ~NODE_TEST;
}

void uiTemplateNodeView(uiLayout *layout, bContext *C, bNodeTree *ntree, bNode *node, bNodeSocket *input)
{
	bNode *tnode;

	if(!ntree)
		return;

	/* clear for cycle check */
	for(tnode=ntree->nodes.first; tnode; tnode=tnode->next)
		tnode->flag &= ~NODE_TEST;

	if(input)
		ui_node_draw_input(layout, C, ntree, node, input, 0);
	else
		ui_node_draw_node(layout, C, ntree, node, 0);
}

