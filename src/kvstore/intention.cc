
#include <sstream>
#include "intention.h"

Intention::Intention(const std::shared_ptr<NodePtr>& snapshot, uint64_t rid) :
  snapshot_(snapshot), rid_(rid), root_(nullptr)
{}

bool Intention::Empty() {
  return root_ == nullptr;
}

void Intention::SetCSN(uint64_t pos)
{
  set_intention_self_csn(root_, pos);
}

void Intention::Serialize(std::string *blob)
{
  int field_index = 0;
  kvstore_proto::Intention i;

  assert(root_ != nullptr);
  serialize_intention(i, root_, field_index);

  i.set_snapshot(snapshot_->csn());

  for (const auto& s : description_)
    i.add_description(s);

  assert(i.IsInitialized());
  assert(i.SerializeToString(blob));
}

void Intention::serialize_node_ptr(kvstore_proto::NodePtr *dst,
    NodePtr& src, const std::string& dir) const
{
  if (src.ref() == Node::Nil()) {
    dst->set_nil(true);
    dst->set_self(false);
    dst->set_csn(0);
    dst->set_off(0);
  } else if (src.ref()->rid() == rid_) {
    dst->set_nil(false);
    dst->set_self(true);
    dst->set_csn(0);
    assert(src.ref()->field_index() >= 0);
    dst->set_off(src.ref()->field_index());
    src.set_offset(src.ref()->field_index());
  } else {
    assert(src.ref() != nullptr);
    dst->set_nil(false);
    dst->set_self(false);
    dst->set_csn(src.csn());
    dst->set_off(src.offset());
  }
}

void Intention::serialize_node(kvstore_proto::Node *dst, NodeRef node,
    int field_index, bool subtree_ro_dependent) const
{
  dst->set_red(node->red());
  dst->set_key(node->key());
  dst->set_val(node->val());
  dst->set_altered(node->altered());
  dst->set_depends(node->depends());
  dst->set_subtree_ro_dependent(subtree_ro_dependent);
  if (node->has_ssv())
    dst->set_ssv(node->ssv());

  assert(node->field_index() == -1);
  // TODO: ideally we could set the field_index when we were
  // balancing/path-building to avoid this process of fixing up the
  // transaction state.
  node->set_field_index(field_index);
  assert(node->field_index() >= 0);

  serialize_node_ptr(dst->mutable_left(), node->left, "left");
  serialize_node_ptr(dst->mutable_right(), node->right, "right");
}

NodeRef Intention::insert_recursive(std::deque<NodeRef>& path,
    std::string key, std::string val, const NodeRef& node, bool& update)
{
  assert(node != nullptr);

  if (node == Node::Nil()) {
    auto nn = std::make_shared<Node>(key, val, true, Node::Nil(),
        Node::Nil(), rid_, -1, false, true, false);
    path.push_back(nn);
    update = false;
    return nn;
  }

  bool less = key < node->key();
  bool equal = !less && key == node->key();

  // this might end up being tricky in the case that updates overlap in the
  // intention and how to manage and update the relevant metadata.
  //
  // either this node with an equal key was found in the snapshot, or it was
  // created by this transaction and updated. if we are making a copy of a
  // node from the snapshot then we set ssv based on the source node's nsv
  // value. however if the node is new within this transaction we leave the
  // ssv undefined, so that case must be handled.
  //
  if (equal) {
    NodeRef copy;
    if (node->rid() == rid_)
      copy = node;
    else {
      copy = Node::Copy(node, rid_);
      copy->set_ssv(node->nsv());
    }
    copy->set_value(val);
    update = true;
    return copy;
  }

  auto child = insert_recursive(path, key, val,
      (less ? node->left.ref() : node->right.ref()),
      update);

  /*
   * the copy_node operation will copy the child node references, as well as
   * the csn/offset for each child node reference. however below the reference
   * is updated without updating the csn/offset, which are fixed later when
   * the intention is build.
   */
  NodeRef copy;
  if (node->rid() == rid_)
    copy = node;
  else {
    copy = Node::Copy(node, rid_);
    copy->set_ssv(node->nsv());
  }

  if (less)
    copy->left.set_ref(child);
  else
    copy->right.set_ref(child);

  path.push_back(copy);

  return copy;
}

template<typename ChildA, typename ChildB >
NodeRef Intention::rotate(NodeRef parent,
    NodeRef child, ChildA child_a, ChildB child_b, NodeRef& root)
{
  // copy over ref and csn/off because we might be moving a pointer that
  // points outside of the current intentino.
  NodePtr grand_child = child_b(child); // copy constructor makes grand_child read-only
  child_b(child) = child_a(grand_child.ref());

  if (root == child) {
    root = grand_child.ref();
  } else if (child_a(parent).ref() == child)
    child_a(parent) = grand_child;
  else
    child_b(parent) = grand_child;

  // we do not update csn/off here because child is always a pointer to a node
  // in the current intention so its csn/off will be updated during intention
  // serialization step.
  assert(child->rid() == rid_);
  child_a(grand_child.ref()).set_ref(child);

  return grand_child.ref();
}

template<typename ChildA, typename ChildB>
void Intention::insert_balance(NodeRef& parent, NodeRef& nn,
    std::deque<NodeRef>& path, ChildA child_a, ChildB child_b,
    NodeRef& root)
{
  assert(path.front() != Node::Nil());
  NodePtr& uncle = child_b(path.front());
  if (uncle.ref()->red()) {
    if (uncle.ref()->rid() != rid_)
      uncle.set_ref(Node::Copy(uncle.ref(), rid_));
    parent->set_red(false);
    uncle.ref()->set_red(false);
    path.front()->set_red(true);
    nn = pop_front(path);
    parent = pop_front(path);
  } else {
    if (nn == child_b(parent).ref()) {
      std::swap(nn, parent);
      rotate(path.front(), nn, child_a, child_b, root);
    }
    auto grand_parent = pop_front(path);
    grand_parent->swap_color(parent);
    rotate(path.front(), grand_parent, child_b, child_a, root);
  }
}

NodeRef Intention::delete_recursive(std::deque<NodeRef>& path,
    std::string key, const NodeRef& node)
{
  assert(node != nullptr);

  if (node == Node::Nil()) {
    return nullptr;
  }

  bool less = key < node->key();
  bool equal = !less && key == node->key();

  if (equal) {
    NodeRef copy;
    if (node->rid() == rid_)
      copy = node;
    else
      copy = Node::Copy(node, rid_);
    path.push_back(copy);
    return copy;
  }

  auto child = delete_recursive(path, key,
      (less ? node->left.ref() : node->right.ref()));

  if (child == nullptr) {
    return child;
  }

  /*
   * the copy_node operation will copy the child node references, as well as
   * the csn/offset for each child node reference. however below the reference
   * is updated without updating the csn/offset, which are fixed later when
   * the intention is build.
   */
  NodeRef copy;
  if (node->rid() == rid_)
    copy = node;
  else
    copy = Node::Copy(node, rid_);

  if (less)
    copy->left.set_ref(child);
  else
    copy->right.set_ref(child);

  path.push_back(copy);

  return copy;
}

void Intention::transplant(NodeRef parent, NodeRef removed,
    NodeRef transplanted, NodeRef& root)
{
  if (parent == Node::Nil()) {
    root = transplanted;
  } else if (parent->left.ref() == removed) {
    parent->left.set_ref(transplanted);
  } else {
    parent->right.set_ref(transplanted);
  }
}

NodeRef Intention::build_min_path(NodeRef node, std::deque<NodeRef>& path)
{
  assert(node != nullptr);
  assert(node->left.ref() != nullptr);
  while (node->left.ref() != Node::Nil()) {
    assert(node->left.ref() != nullptr);
    if (node->left.ref()->rid() != rid_)
      node->left.set_ref(Node::Copy(node->left.ref(), rid_));
    path.push_front(node);
    node = node->left.ref();
    assert(node != nullptr);
  }
  return node;
}

template<typename ChildA, typename ChildB>
void Intention::mirror_remove_balance(NodeRef& extra_black, NodeRef& parent,
    std::deque<NodeRef>& path, ChildA child_a, ChildB child_b, NodeRef& root)
{
  NodeRef brother = child_b(parent).ref();

  if (brother->red()) {
    if (brother->rid() != rid_)
      child_b(parent).set_ref(Node::Copy(brother, rid_));
    else
      child_b(parent).set_ref(brother);
    brother = child_b(parent).ref();

    brother->swap_color(parent);
    rotate(path.front(), parent, child_a, child_b, root);
    path.push_front(brother);

    brother = child_b(parent).ref();
  }

  assert(brother != nullptr);

  assert(brother->left.ref() != nullptr);
  assert(brother->right.ref() != nullptr);

  if (!brother->left.ref()->red() && !brother->right.ref()->red()) {
    if (brother->rid() != rid_)
      child_b(parent).set_ref(Node::Copy(brother, rid_));
    else
      child_b(parent).set_ref(brother);
    brother = child_b(parent).ref();

    brother->set_red(true);
    extra_black = parent;
    parent = pop_front(path);
  } else {
    if (!child_b(brother).ref()->red()) {
      if (brother->rid() != rid_)
        child_b(parent).set_ref(Node::Copy(brother, rid_));
      else
        child_b(parent).set_ref(brother);
      brother = child_b(parent).ref();

      if (child_a(brother).ref()->rid() != rid_)
        child_a(brother).set_ref(Node::Copy(child_a(brother).ref(), rid_));
      brother->swap_color(child_a(brother).ref());
      brother = rotate(parent, brother, child_b, child_a, root);
    }

    if (brother->rid() != rid_)
      child_b(parent).set_ref(Node::Copy(brother, rid_));
    else
      child_b(parent).set_ref(brother);
    brother = child_b(parent).ref();

    if (child_b(brother).ref()->rid() != rid_)
      child_b(brother).set_ref(Node::Copy(child_b(brother).ref(), rid_));
    brother->set_red(parent->red());
    parent->set_red(false);
    child_b(brother).ref()->set_red(false);
    rotate(path.front(), parent, child_a, child_b, root);

    extra_black = root;
    parent = Node::Nil();
  }
}

void Intention::balance_delete(NodeRef extra_black,
    std::deque<NodeRef>& path, NodeRef& root)
{
  auto parent = pop_front(path);

  assert(extra_black != nullptr);
  assert(root != nullptr);
  assert(parent != nullptr);

  //db_->cache_.ResolveNodePtr(parent->left);
  //assert(parent->left.ref() != nullptr);

  while (extra_black != root && !extra_black->red()) {
    if (parent->left.ref() == extra_black)
      mirror_remove_balance(extra_black, parent, path, left, right, root);
    else
      mirror_remove_balance(extra_black, parent, path, right, left, root);
  }

  NodeRef new_node;
  if (extra_black->rid() == rid_)
    new_node = extra_black;
  else
    new_node = Node::Copy(extra_black, rid_);
  transplant(parent, extra_black, new_node, root);

  /*
   * new_node may point to nil, and this call sets the color to black (nil is
   * always black, so this is OK). however we treat nil as read-only, so we
   * only make this call that may throw a non-read-only assertion failure for
   * non-nil nodes.
   *
   * TODO: is there something fundmentally wrong with the algorithm that
   * new_node may even point to nil?
   */
  if (new_node != Node::Nil())
    new_node->set_red(false);
}

void Intention::serialize_intention(kvstore_proto::Intention& out,
    NodeRef node, int& field_index, bool *parent_subtree_ro_dependent)
{
  assert(node != nullptr);

  if (node == Node::Nil() || node->rid() != rid_)
    return;

  // will be set ot false if any descendent node has altered = true
  bool this_subtree_ro_dependent = true;

  serialize_intention(out, node->left.ref(), field_index, &this_subtree_ro_dependent);
  serialize_intention(out, node->right.ref(), field_index, &this_subtree_ro_dependent);

  /*
   * a new node starts off with ssv being undefined--it doesn't have a source.
   * however later when this node's nsv is referenced, it will return ssv when
   * its subtree-ro-depdenent flag is set. since the subtree-ro-dependent flag
   * seems to be a function of the final position of the node in the tree
   * before serialization, there seems to be a disconnect. perhaps a node that
   * is "new" or doesn't have an ssv defined returns its vn?
   */
#if 0
  if ((node->left.ref() == Node::Nil() || node->left.ref()->rid() != rid_) &&
      (node->right.ref() == Node::Nil() || node->right.ref()->rid() != rid_))
    this_subtree_ro_dependent = false;
#endif

  // new serialized node in the intention
  kvstore_proto::Node *dst = out.add_tree();
  serialize_node(dst, node, field_index, this_subtree_ro_dependent);
  field_index++;

  if (parent_subtree_ro_dependent &&
      (node->altered() || !this_subtree_ro_dependent))
    *parent_subtree_ro_dependent = false;
}

void Intention::Put(const std::string& key, const std::string& val)
{
  /*
   * build copy of path to new node
   */
  std::deque<NodeRef> path;

  bool update;
  auto base_root = root_ == nullptr ? snapshot_->ref() : root_;
  auto root = insert_recursive(path, key, val, base_root, update);
  assert(root != nullptr);

  /*
   * update is true when the key is already in the tree. in this case the
   * structure of the tree did not change--a path in the tree was copied--and
   * we can avoid any rebalancing.
   */
  if (update) {
    std::stringstream ss;
    ss << "update: " << key;
    description_.emplace_back(ss.str());

    assert(!root->red());
    root_ = root;

    return;
  }

  std::stringstream ss;
  ss << "put: " << key;
  description_.emplace_back(ss.str());

  path.push_back(Node::Nil());
  assert(path.size() >= 2);

  /*
   * balance the tree
   */
  auto nn = pop_front(path);
  auto parent = pop_front(path);

  while (parent->red()) {
    assert(!path.empty());
    auto grand_parent = path.front();
    if (grand_parent->left.ref() == parent)
      insert_balance(parent, nn, path, left, right, root);
    else
      insert_balance(parent, nn, path, right, left, root);
  }

  root->set_red(false);

  assert(root != nullptr);
  root_ = root;
}

void Intention::Delete(std::string key)
{
  std::deque<NodeRef> path;

  std::stringstream ss;
  ss << "del: " << key;
  description_.emplace_back(ss.str());

  auto base_root = root_ == nullptr ? snapshot_->ref() : root_;
  auto root = delete_recursive(path, key, base_root);
  if (root == nullptr) {
    return;
  }

  //roots.push_back(node);
  path.push_back(Node::Nil());
  assert(path.size() >= 2);

  /*
   * remove and balance
   */
  auto removed = path.front();
  assert(removed != nullptr);
  assert(removed->key() == key);

  auto transplanted = removed->right.ref();
  assert(transplanted != nullptr);

  if (removed->left.ref() == Node::Nil()) {
    path.pop_front();
    transplant(path.front(), removed, transplanted, root);
    assert(transplanted != nullptr);
  } else if (removed->right.ref() == Node::Nil()) {
    path.pop_front();
    assert(removed->left.ref() != nullptr);
    transplanted = removed->left.ref();
    transplant(path.front(), removed, transplanted, root);
    assert(transplanted != nullptr);
  } else {
    assert(transplanted != nullptr);
    auto temp = removed;
    if (removed->right.ref()->rid() != rid_)
      removed->right.set_ref(Node::Copy(removed->right.ref(), rid_));
    removed = build_min_path(removed->right.ref(), path);
    transplanted = removed->right.ref();
    assert(transplanted != nullptr);

    //temp->key = std::move(removed->key);
    //temp->val = std::move(removed->val);
    temp->steal_payload(removed);

    transplant(path.front(), removed, transplanted, root);
    assert(transplanted != nullptr);
  }

  if (!removed->red())
    balance_delete(transplanted, path, root);

  assert(root != nullptr);
  root_ = root;
}

void Intention::set_intention_self_csn_recursive(uint64_t rid,
    NodeRef node, uint64_t pos)
{
  if (node == Node::Nil() || node->rid() != rid)
    return;

  if (node->right.ref() != Node::Nil() && node->right.ref()->rid() == rid) {
    node->right.set_csn(pos);
  }

  if (node->left.ref() != Node::Nil() && node->left.ref()->rid() == rid) {
    node->left.set_csn(pos);
  }

  set_intention_self_csn_recursive(rid, node->right.ref(), pos);
  set_intention_self_csn_recursive(rid, node->left.ref(), pos);
}

void Intention::set_intention_self_csn(NodeRef root, uint64_t pos)
{
  set_intention_self_csn_recursive(root->rid(), root, pos);
}

