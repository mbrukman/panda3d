// Filename: eggOptchar.cxx
// Created by:  drose (18Jul03)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) 2001, Disney Enterprises, Inc.  All rights reserved
//
// All use of this software is subject to the terms of the Panda 3d
// Software license.  You should have received a copy of this license
// along with this source code; you will also find a current copy of
// the license at http://www.panda3d.org/license.txt .
//
// To contact the maintainers of this program write to
// panda3d@yahoogroups.com .
//
////////////////////////////////////////////////////////////////////

#include "eggOptchar.h"
#include "eggOptcharUserData.h"
#include "vertexMembership.h"

#include "eggJointData.h"
#include "eggSliderData.h"
#include "eggCharacterCollection.h"
#include "eggCharacterData.h"
#include "eggBackPointer.h"
#include "eggGroupNode.h"
#include "eggVertexPool.h"
#include "string_utils.h"
#include "dcast.h"
#include "pset.h"

#include <algorithm>

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::Constructor
//       Access: Public
//  Description:
////////////////////////////////////////////////////////////////////
EggOptchar::
EggOptchar() {
  add_path_replace_options();
  add_path_store_options();
  add_normals_options();
  add_transform_options();

  set_program_description
    ("egg-optchar performs basic optimizations of a character model "
     "and its associated animations, primarily by analyzing the "
     "animation tables and removing unneeded joints and/or morphs.  "
     "It can also perform basic restructuring operations on the "
     "character hierarchy.");

  add_option
    ("ls", "", 0,
     "List the joint hierarchy instead of performing any operations.",
     &EggOptchar::dispatch_none, &_list_hierarchy);

  add_option
    ("lp", "", 0,
     "List the existing joint hierarchy as a series of -p joint,parent "
     "commands, suitable for pasting into an egg-optchar command line.",
     &EggOptchar::dispatch_none, &_list_hierarchy_p);

  add_option
    ("keep", "joint[,joint...]", 0,
     "Keep the named joints (or sliders) in the character, even if they do "
     "not appear to be needed by the animation.",
     &EggOptchar::dispatch_vector_string_comma, NULL, &_keep_components);

  add_option
    ("expose", "joint[,joint...]", 0,
     "Expose the named joints by flagging them with a DCS attribute, so "
     "each one can be found in the scene graph when the character is loaded, "
     "and objects can be parented to it.  This implies -keep.",
     &EggOptchar::dispatch_vector_string_comma, NULL, &_expose_components);

  add_option
    ("keepall", "", 0,
     "Keep all joints and sliders in the character.",
     &EggOptchar::dispatch_none, &_keep_all);
  
  add_option
    ("p", "joint,parent", 0,
     "Moves the named joint under the named parent joint.  Use "
     "\"-p joint,\" to reparent a joint to the root.  The joint transform "
     "is recomputed appropriately under its new parent so that the animation "
     "is not affected (the effect is similar to NodePath::wrt_reparent_to).",
     &EggOptchar::dispatch_vector_string_pair, NULL, &_reparent_joints);

  add_option
    ("q", "quantum", 0,
     "Quantize joint membership values to the given unit.  This is "
     "the smallest significant change in joint membership.  The "
     "default is 0.01; specifying 0 means to preserve the original "
     "values.",
     &EggOptchar::dispatch_double, NULL, &_vref_quantum);

  _vref_quantum = 0.01;
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::run
//       Access: Public
//  Description:
////////////////////////////////////////////////////////////////////
void EggOptchar::
run() {
  // We have to apply the user-specified reparent requests first,
  // before we even analyze the joints.  This is because reparenting
  // the joints may change their properties.
  if (apply_user_reparents()) {
    cerr << "Reparenting hierarchy.\n";
    // So we'll have to call do_reparent() twice.  It seems wasteful,
    // but it really is necessary, and it's not that bad.
    do_reparent();
  }

  int num_characters = _collection->get_num_characters();
  int ci;

  // Now we can analyze the joints for their properties.
  for (ci = 0; ci < num_characters; ci++) {
    EggCharacterData *char_data = _collection->get_character(ci);
    analyze_joints(char_data->get_root_joint());
    analyze_sliders(char_data);
  }

  if (_list_hierarchy) {
    for (ci = 0; ci < num_characters; ci++) {
      EggCharacterData *char_data = _collection->get_character(ci);
      nout << "Character: " << char_data->get_name() << "\n";
      list_joints(char_data->get_root_joint(), 0);
      list_scalars(char_data);
      nout << char_data->get_num_joints() << " joints.\n";
    }

  } else if (_list_hierarchy_p) {
    for (ci = 0; ci < num_characters; ci++) {
      EggCharacterData *char_data = _collection->get_character(ci);
      nout << "Character: " << char_data->get_name() << "\n";
      list_joints_p(char_data->get_root_joint());
      // A newline to cout is needed after the above call.
      cout << "\n";
      nout << char_data->get_num_joints() << " joints.\n";
    }

  } else {
    // The meat of the program: determine which joints are to be
    // removed, and then actually remove them.
    determine_removed_components();
    if (process_joints()) {
      do_reparent();
    }

    // Quantize the vertex memberships.  We call this even if
    // _vref_quantum is 0, because this also normalizes the vertex
    // memberships.
    quantize_vertices();

    // We currently do not implement optimizing morph sliders.  Need
    // to add this at some point; it's quite easy.  Identity and empty
    // morph sliders can simply be removed, while static sliders need
    // to be applied to the vertices and then removed.

    write_eggs();
  }
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::handle_args
//       Access: Protected, Virtual
//  Description: Does something with the additional arguments on the
//               command line (after all the -options have been
//               parsed).  Returns true if the arguments are good,
//               false otherwise.
////////////////////////////////////////////////////////////////////
bool EggOptchar::
handle_args(ProgramBase::Args &args) {
  if (_list_hierarchy) {
    _read_only = true;
  }

  return EggCharacterFilter::handle_args(args);
}

////////////////////////////////////////////////////////////////////
//     Function: ProgramBase::dispatch_vector_string_pair
//       Access: Protected, Static
//  Description: Standard dispatch function for an option that takes
//               a pair of string parameters.  The data pointer is to
//               StringPairs vector; the pair will be pushed onto the
//               end of the vector.
////////////////////////////////////////////////////////////////////
bool EggOptchar::
dispatch_vector_string_pair(const string &opt, const string &arg, void *var) {
  StringPairs *ip = (StringPairs *)var;

  vector_string words;
  tokenize(arg, words, ",");

  if (words.size() == 2) {
    StringPair sp;
    sp._a = words[0];
    sp._b = words[1];
    ip->push_back(sp);

  } else {
    nout << "-" << opt
         << " requires a pair of strings separated by a comma.\n";
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::determine_removed_components
//       Access: Private
//  Description: Flag all joints and sliders that should be removed
//               for optimization purposes.
////////////////////////////////////////////////////////////////////
void EggOptchar::
determine_removed_components() {
  typedef pset<string> Names;
  Names keep_names;
  Names expose_names;
  Names names_used;

  vector_string::const_iterator si;
  for (si = _keep_components.begin(); si != _keep_components.end(); ++si) {
    keep_names.insert(*si);
  }
  for (si = _expose_components.begin(); si != _expose_components.end(); ++si) {
    keep_names.insert(*si);
    expose_names.insert(*si);
  }

  // We always keep the root joint, which has no name.
  keep_names.insert("");

  int num_characters = _collection->get_num_characters();
  for (int ci = 0; ci < num_characters; ci++) {
    EggCharacterData *char_data = _collection->get_character(ci);
    int num_components = char_data->get_num_components();
    for (int i = 0; i < num_components; i++) {
      EggComponentData *comp_data = char_data->get_component(i);
      EggOptcharUserData *user_data = 
        DCAST(EggOptcharUserData, comp_data->get_user_data());

      const string &name = comp_data->get_name();
      if (_keep_all || keep_names.find(name) != keep_names.end()) {
        // Keep this component.
        names_used.insert(name);

        if (expose_names.find(name) != expose_names.end()) {
          // In fact, expose it.
          user_data->_flags |= EggOptcharUserData::F_expose;
        }

      } else {
        // Remove this component if it's unanimated or empty.
        if ((user_data->_flags & (EggOptcharUserData::F_static | EggOptcharUserData::F_empty)) != 0) {
          user_data->_flags |= EggOptcharUserData::F_remove;
        }
      }
    }
  }

  // Go back and tell the user about component names we didn't use,
  // just to be helpful.
  for (si = _keep_components.begin(); si != _keep_components.end(); ++si) {
    const string &name = (*si);
    if (names_used.find(name) == names_used.end()) {
      nout << "No such joint: " << name << "\n";
    }
  }
  for (si = _expose_components.begin(); si != _expose_components.end(); ++si) {
    const string &name = (*si);
    if (names_used.find(name) == names_used.end()) {
      nout << "No such joint: " << name << "\n";
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::process_joints
//       Access: Private
//  Description: Effects the actual removal of joints flagged for
//               removal by reparenting the hierarchy appropriately.
//               Returns true if any joints are removed, false
//               otherwise.
////////////////////////////////////////////////////////////////////
bool EggOptchar::
process_joints() {
  bool removed_any = false;
  int num_characters = _collection->get_num_characters();
  for (int ci = 0; ci < num_characters; ci++) {
    EggCharacterData *char_data = _collection->get_character(ci);
    int num_joints = char_data->get_num_joints();
    
    int num_static = 0;
    int num_empty = 0;
    int num_identity = 0;
    int num_kept = 0;
    
    for (int i = 0; i < num_joints; i++) {
      EggJointData *joint_data = char_data->get_joint(i);
      EggOptcharUserData *user_data = 
        DCAST(EggOptcharUserData, joint_data->get_user_data());
      
      EggJointData *best_parent = find_best_parent(joint_data->get_parent());
      
      if ((user_data->_flags & EggOptcharUserData::F_remove) != 0) {
        // This joint will be removed, so reparent it to nothing.
        joint_data->reparent_to((EggJointData *)NULL);
        
        // Move the vertices associated with this joint into its
        // parent.
        joint_data->move_vertices_to(best_parent);
        
        // Determine what kind of node it is we're removing, for the
        // user's information.
        if ((user_data->_flags & EggOptcharUserData::F_identity) != 0) {
          num_identity++;
        } else if ((user_data->_flags & EggOptcharUserData::F_static) != 0) {
          num_static++;
        } else if ((user_data->_flags & EggOptcharUserData::F_empty) != 0) {
          num_empty++;
        }
        removed_any = true;

      } else {
        // This joint will be preserved, but maybe its parent will
        // change.
        joint_data->reparent_to(best_parent);
        if ((user_data->_flags & EggOptcharUserData::F_expose) != 0) {
          joint_data->expose();
        }
        num_kept++;
      }
    }

    if (num_joints == num_kept) {
      nout << char_data->get_name() << ": keeping " << num_joints
           << " joints.\n";
    } else {
      nout << char_data->get_name() << ": of " << num_joints 
           << " joints, removing " << num_identity << " identity, "
           << num_static << " static, and " << num_empty
           << " empty joints, leaving " << num_kept << ".\n";
    }
  }

  return removed_any;
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::find_best_parent
//       Access: Private
//  Description: Searches for this first joint at this level or above
//               that is not scheduled to be removed.  This is the
//               joint that the first child of this joint should be
//               reparented to.
////////////////////////////////////////////////////////////////////
EggJointData *EggOptchar::
find_best_parent(EggJointData *joint_data) const {
  EggOptcharUserData *user_data = 
    DCAST(EggOptcharUserData, joint_data->get_user_data());

  if ((user_data->_flags & EggOptcharUserData::F_remove) != 0) {
    // Keep going.
    nassertr(joint_data->get_parent() != (EggJointData *)NULL, NULL);
    return find_best_parent(joint_data->get_parent());
  }

  // This is the one!
  return joint_data;
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::apply_user_reparents
//       Access: Private
//  Description: Reparents all the joints that the user suggested on
//               the command line.  Returns true if any operations
//               were performed, false otherwise.
////////////////////////////////////////////////////////////////////
bool EggOptchar::
apply_user_reparents() {
  bool did_anything = false;
  int num_characters = _collection->get_num_characters();

  StringPairs::const_iterator spi;
  for (spi = _reparent_joints.begin(); spi != _reparent_joints.end(); ++spi) {
    const StringPair &p = (*spi);

    for (int ci = 0; ci < num_characters; ci++) {
      EggCharacterData *char_data = _collection->get_character(ci);
      EggJointData *node_a = char_data->find_joint(p._a);
      EggJointData *node_b = char_data->get_root_joint();
      if (!p._b.empty()) {
        node_b = char_data->find_joint(p._b);
      }

      if (node_a == (EggJointData *)NULL) {
        nout << "No joint named " << p._a << " in " << char_data->get_name()
             << ".\n";
      } else if (node_b == (EggJointData *)NULL) {
        nout << "No joint named " << p._b << " in " << char_data->get_name()
             << ".\n";
      } else {
        node_a->reparent_to(node_b);
        did_anything = true;
      }
    }
  }

  return did_anything;
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::analyze_joints
//       Access: Private
//  Description: Recursively walks the joint hierarchy for a
//               particular character, indentifying properties of each
//               joint.
////////////////////////////////////////////////////////////////////
void EggOptchar::
analyze_joints(EggJointData *joint_data) {
  PT(EggOptcharUserData) user_data = new EggOptcharUserData;
  joint_data->set_user_data(user_data);

  // Analyze the table of matrices for this joint, checking to see if
  // they're all the same across all frames, or if any of them are
  // different; also look for empty joints (that control no vertices).
  int num_mats = 0;
  bool different_mat = false;
  bool has_vertices = false;

  int num_models = joint_data->get_num_models();
  for (int i = 0; i < num_models; i++) {
    if (joint_data->has_model(i)) {
      EggBackPointer *model = joint_data->get_model(i);
      if (model->has_vertices()) {
        has_vertices = true;
      }

      int num_frames = joint_data->get_num_frames(i);

      int f;
      for (f = 0; f < num_frames && !different_mat; f++) {
        LMatrix4d mat = joint_data->get_frame(i, f);
        num_mats++;
        if (num_mats == 1) {
          // This is the first matrix.
          user_data->_static_mat = mat;

        } else {
          // This is a second or later matrix.
          if (!mat.almost_equal(user_data->_static_mat)) {
            // It's different than the first one.
            different_mat = true;
          }
        }
      }
    }
  }

  if (!different_mat) {
    // All the mats are the same for this joint.
    user_data->_flags |= EggOptcharUserData::F_static;

    if (num_mats == 0 || 
        user_data->_static_mat.almost_equal(LMatrix4d::ident_mat())) {
      // It's not only static, but it's the identity matrix.
      user_data->_flags |= EggOptcharUserData::F_identity;
    }
  }

  if (!has_vertices) {
    // There are no vertices in this joint.
    user_data->_flags |= EggOptcharUserData::F_empty;
  }

  int num_children = joint_data->get_num_children();
  for (int i = 0; i < num_children; i++) {
    analyze_joints(joint_data->get_child(i));
  }
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::analyze_sliders
//       Access: Private
//  Description: Linearly walks the slider list for a particular
//               character, indentifying properties of each slider.
////////////////////////////////////////////////////////////////////
void EggOptchar::
analyze_sliders(EggCharacterData *char_data) {
  int num_sliders = char_data->get_num_sliders();
  for (int si = 0; si < num_sliders; si++) {
    EggSliderData *slider_data = char_data->get_slider(si);

    PT(EggOptcharUserData) user_data = new EggOptcharUserData;
    slider_data->set_user_data(user_data);

    // Analyze the table of values for this slider, checking to see if
    // they're all the same across all frames, or if any of them are
    // different; also look for empty sliders (that control no vertices).
    int num_values = 0;
    bool different_value = false;
    bool has_vertices = false;

    int num_models = slider_data->get_num_models();
    for (int i = 0; i < num_models; i++) {
      if (slider_data->has_model(i)) {
        EggBackPointer *model = slider_data->get_model(i);
        if (model->has_vertices()) {
          has_vertices = true;
        }
        
        int num_frames = slider_data->get_num_frames(i);
        
        int f;
        for (f = 0; f < num_frames && !different_value; f++) {
          double value = slider_data->get_frame(i, f);
          num_values++;
          if (num_values == 1) {
            // This is the first value.
            user_data->_static_value = value;
            
          } else {
            // This is a second or later value.
            if (!IS_NEARLY_EQUAL(value, user_data->_static_value)) {
              // It's different than the first one.
              different_value = true;
            }
          }
        }
      }
    }
    
    if (!different_value) {
      // All the values are the same for this slider.
      user_data->_flags |= EggOptcharUserData::F_static;
      
      if (num_values == 0 || IS_NEARLY_EQUAL(user_data->_static_value, 0.0)) {
        // It's not only static, but it's the identity value.
        user_data->_flags |= EggOptcharUserData::F_identity;
      }
    }
    
    if (!has_vertices) {
      // There are no vertices in this slider.
      user_data->_flags |= EggOptcharUserData::F_empty;
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::list_joints
//       Access: Private
//  Description: Outputs a list of the joint hierarchy.
////////////////////////////////////////////////////////////////////
void EggOptchar::
list_joints(EggJointData *joint_data, int indent_level) {
  // Don't list the root joint, which is artificially created when the
  // character is loaded.  Instead, list each child as it is
  // encountered.

  int num_children = joint_data->get_num_children();
  for (int i = 0; i < num_children; i++) {
    EggJointData *child_data = joint_data->get_child(i);
    describe_component(child_data, indent_level);

    list_joints(child_data, indent_level + 2);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::list_joints_p
//       Access: Private
//  Description: Outputs a list of the joint hierarchy as a series of
//               -p joint,parent commands.
////////////////////////////////////////////////////////////////////
void EggOptchar::
list_joints_p(EggJointData *joint_data) {
  // As above, don't list the root joint.

  int num_children = joint_data->get_num_children();
  for (int i = 0; i < num_children; i++) {
    EggJointData *child_data = joint_data->get_child(i);
    // We send output to cout instead of nout to avoid the
    // word-wrapping, and also to allow the user to redirect this
    // easily to a file.
    cout << " -p " << child_data->get_name() << "," 
         << joint_data->get_name();

    list_joints_p(child_data);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::list_scalars
//       Access: Private
//  Description: Outputs a list of the scalars.
////////////////////////////////////////////////////////////////////
void EggOptchar::
list_scalars(EggCharacterData *char_data) {
  int num_sliders = char_data->get_num_sliders();
  for (int si = 0; si < num_sliders; si++) {
    EggSliderData *slider_data = char_data->get_slider(si);
    describe_component(slider_data, 0);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::describe_component
//       Access: Private
//  Description: Describes one particular slider or joint.
////////////////////////////////////////////////////////////////////
void EggOptchar::
describe_component(EggComponentData *comp_data, int indent_level) {
  // We use cout instead of nout so the user can easily redirect this
  // to a file.
  indent(cout, indent_level)
    << comp_data->get_name();
  
  EggOptcharUserData *user_data = 
    DCAST(EggOptcharUserData, comp_data->get_user_data());
  if (user_data->is_identity()) {
    cout << " (identity)";
  } else if (user_data->is_static()) {
    cout << " (static)";
  }
  if (user_data->is_empty()) {
    cout << " (empty)";
  }
  cout << "\n";
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::do_reparent
//       Access: Private
//  Description: Performs all of the queued up reparenting operations.
////////////////////////////////////////////////////////////////////
void EggOptchar::
do_reparent() {
  int num_characters = _collection->get_num_characters();
  for (int ci = 0; ci < num_characters; ci++) {
    EggCharacterData *char_data = _collection->get_character(ci);
    char_data->do_reparent();
  }
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::quantize_vertices
//       Access: Private
//  Description: Walks through all of the loaded egg files, looking
//               for vertices whose joint memberships are then
//               quantized according to _vref_quantum.
////////////////////////////////////////////////////////////////////
void EggOptchar::
quantize_vertices() {
  Eggs::iterator ei;
  for (ei = _eggs.begin(); ei != _eggs.end(); ++ei) {
    quantize_vertices(*ei);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::quantize_vertices
//       Access: Private
//  Description: Recursively walks through the indicated egg
//               hierarchy, looking for vertices whose joint
//               memberships are then quantized according to
//               _vref_quantum.
////////////////////////////////////////////////////////////////////
void EggOptchar::
quantize_vertices(EggNode *egg_node) {
  if (egg_node->is_of_type(EggVertexPool::get_class_type())) {
    EggVertexPool *vpool = DCAST(EggVertexPool, egg_node);
    EggVertexPool::iterator vi;
    for (vi = vpool->begin(); vi != vpool->end(); ++vi) {
      quantize_vertex(*vi);
    }
    
  } else if (egg_node->is_of_type(EggGroupNode::get_class_type())) {
    EggGroupNode *group = DCAST(EggGroupNode, egg_node);
    EggGroupNode::iterator ci;
    for (ci = group->begin(); ci != group->end(); ++ci) {
      quantize_vertices(*ci);
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: EggOptchar::quantize_vertex
//       Access: Private
//  Description: Quantizes the indicated vertex's joint membership.
////////////////////////////////////////////////////////////////////
void EggOptchar::
quantize_vertex(EggVertex *egg_vertex) {
  if (egg_vertex->gref_size() == 0) {
    // Never mind on this vertex.
    return;
  }

  // First, get a copy of the existing membership.
  VertexMemberships memberships;
  EggVertex::GroupRef::const_iterator gi;
  double net_membership = 0.0;
  for (gi = egg_vertex->gref_begin(); gi != egg_vertex->gref_end(); ++gi) {
    EggGroup *group = (*gi);
    double membership = group->get_vertex_membership(egg_vertex);
    memberships.push_back(VertexMembership(group, membership));
    net_membership += membership;
  }
  nassertv(net_membership != 0.0);

  // Now normalize all the memberships so the net membership is 1.0,
  // and then quantize the result (if the user so requested).
  double factor = 1.0 / net_membership;
  net_membership = 0.0;
  VertexMemberships::iterator mi;
  VertexMemberships::iterator largest = memberships.begin();

  for (mi = memberships.begin(); mi != memberships.end(); ++mi) {
    if ((*largest) < (*mi)) {
      // Remember the largest membership value, so we can readjust it
      // at the end.
      largest = mi;
    }

    double value = (*mi)._membership * factor;
    if (_vref_quantum != 0.0) {
      value = floor(value / _vref_quantum + 0.5) * _vref_quantum;
    }
    (*mi)._membership = value;

    net_membership += value;
  }

  // The the largest membership value gets corrected again by the
  // roundoff error.
  (*largest)._membership += 1.0 - net_membership;

  // Finally, walk back through and apply these computed values to the
  // vertex.
  for (mi = memberships.begin(); mi != memberships.end(); ++mi) {
    (*mi)._group->set_vertex_membership(egg_vertex, (*mi)._membership);
  }
}


int main(int argc, char *argv[]) {
  EggOptchar prog;
  prog.parse_command_line(argc, argv);
  prog.run();
  return 0;
}
