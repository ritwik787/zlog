#include "db_impl.h"
#include <sstream>

DBImpl::DBImpl(zlog::Log *log) :
  root_(Node::Nil(), this, true), root_pos_(0), log_(log), cache_(this),
  stop_(false),
  cur_txn_(nullptr)
{
  validate_rb_tree(root_);

  txn_finisher_ = std::thread(&DBImpl::TransactionFinisher, this);
}

int DB::Open(zlog::Log *log, bool create_if_empty, DB **db)
{
  uint64_t tail;
  int ret = log->CheckTail(&tail);
  assert(ret == 0);

  // empty log
  if (tail == 0) {
    if (!create_if_empty)
      return -EINVAL;

    std::string blob;
    kvstore_proto::Intention intention;
    intention.set_snapshot(-1);
    assert(intention.IsInitialized());
    assert(intention.SerializeToString(&blob));

    ret = log->Append(blob, &tail);
    assert(ret == 0);
    assert(tail == 0);

    ret = log->CheckTail(&tail);
    assert(ret == 0);
    assert(tail == 1);
  }

  DBImpl *impl = new DBImpl(log);
  *db = impl;

  return 0;
}

DB::~DB()
{
}

DBImpl::~DBImpl()
{
  {
    std::lock_guard<std::mutex> l(lock_);
    stop_ = true;
  }
  txn_finisher_cond_.notify_one();
  txn_finisher_.join();
  cache_.Stop();
}

std::ostream& operator<<(std::ostream& out, const SharedNodeRef& n)
{
  out << "node(" << n.get() << "):" << n->key().ToString() << ": ";
  out << (n->red() ? "red " : "blk ");
  out << "fi " << n->field_index() << " ";
  out << "left=[p" << n->left.csn() << ",o" << n->left.offset() << ",";
  if (n->left.ref_notrace() == Node::Nil())
    out << "nil";
  else
    out << n->left.ref_notrace().get();
  out << "] ";
  out << "right=[p" << n->right.csn() << ",o" << n->right.offset() << ",";
  if (n->right.ref_notrace() == Node::Nil())
    out << "nil";
  else
    out << n->right.ref_notrace().get();
  out << "] ";
  return out;
}

std::ostream& operator<<(std::ostream& out, const kvstore_proto::NodePtr& p)
{
  out << "[n" << p.nil() << ",s" << p.self()
    << ",p" << p.csn() << ",o" << p.off() << "]";
  return out;
}

std::ostream& operator<<(std::ostream& out, const kvstore_proto::Node& n)
{
  out << "key " << n.key() << " val " << n.val() << " ";
  out << (n.red() ? "red" : "blk") << " ";
  out << "left " << n.left() << " right " << n.right();
  return out;
}

std::ostream& operator<<(std::ostream& out, const kvstore_proto::Intention& i)
{
  out << "- intention tree_size = " << i.tree_size() << std::endl;
  for (int idx = 0; idx < i.tree_size(); idx++) {
    out << "  " << idx << ": " << i.tree(idx) << std::endl;
  }
  return out;
}

uint64_t DBImpl::root_id_ = 928734;

void DBImpl::write_dot_null(std::ostream& out,
    SharedNodeRef node, uint64_t& nullcount)
{
  nullcount++;
  out << "null" << nullcount << " [shape=point];"
    << std::endl;
  out << "\"" << node.get() << "\" -> " << "null"
    << nullcount << " [label=\"nil\"];" << std::endl;
}

void DBImpl::write_dot_node(std::ostream& out,
    SharedNodeRef parent, NodePtr& child, const std::string& dir)
{
  out << "\"" << parent.get() << "\":" << dir << " -> ";
  out << "\"" << child.ref_notrace().get() << "\"";
  out << " [label=\"" << child.csn() << ":"
    << child.offset() << "\"];" << std::endl;
}

void DBImpl::write_dot_recursive(std::ostream& out, uint64_t rid,
    SharedNodeRef node, uint64_t& nullcount, bool scoped)
{
  if (scoped && node->rid() != rid)
    return;

  out << "\"" << node.get() << "\" ["
    << "label=\"" << node->key().ToString() << "_" << node->val().ToString() << "\",style=filled,"
    << "fillcolor=" << (node->red() ? "red" :
        "black,fontcolor=white")
    << "]" << std::endl;

  assert(node->left.ref_notrace() != nullptr);
  if (node->left.ref_notrace() == Node::Nil())
    write_dot_null(out, node, nullcount);
  else {
    write_dot_node(out, node, node->left, "sw");
    write_dot_recursive(out, rid, node->left.ref_notrace(), nullcount, scoped);
  }

  assert(node->right.ref_notrace() != nullptr);
  if (node->right.ref_notrace() == Node::Nil())
    write_dot_null(out, node, nullcount);
  else {
    write_dot_node(out, node, node->right, "se");
    write_dot_recursive(out, rid, node->right.ref_notrace(), nullcount, scoped);
  }
}

void DBImpl::_write_dot(std::ostream& out, SharedNodeRef root,
    uint64_t& nullcount, bool scoped)
{
  assert(root != nullptr);
  write_dot_recursive(out, root->rid(),
      root, nullcount, scoped);
}

void DBImpl::write_dot(std::ostream& out, bool scoped)
{
  auto root = root_;
  uint64_t nullcount = 0;
  out << "digraph ptree {" << std::endl;
  _write_dot(out, root.ref_notrace(), nullcount, scoped);
  out << "}" << std::endl;
}

void DBImpl::write_dot_history(std::ostream& out,
    std::vector<Snapshot*>& snapshots)
{
  uint64_t trees = 0;
  uint64_t nullcount = 0;
  out << "digraph ptree {" << std::endl;
  std::string prev_root = "";

  for (auto it = snapshots.cbegin(); it != snapshots.end(); it++) {

    // build sub-graph label
    std::stringstream label;
    label << "label = \"root: " << (*it)->seq;
    for (const auto& s : (*it)->desc)
      label << "\n" << s;
    label << "\"";

    out << "subgraph cluster_" << trees++ << " {" << std::endl;
    auto ref = (*it)->root.ref_notrace();
    if (ref == Node::Nil()) {
      out << "null" << ++nullcount << " [label=nil];" << std::endl;
    } else {
      _write_dot(out, ref, nullcount, true);
    }

#if 0
    if (prev_root != "")
      out << "\"" << prev_root << "\" -> \"" << it->root.get() << "\" [style=invis];" << std::endl;
    std::stringstream ss;
    ss << it->root.get();
    prev_root = ss.str();
#endif
    out << label.str() << std::endl;
    out << "}" << std::endl;
  }
  out << "}" << std::endl;
}

void DBImpl::print_node(SharedNodeRef node)
{
  if (node == Node::Nil())
    std::cout << "nil:" << (node->red() ? "r" : "b");
  else
    std::cout << node->key().ToString() << ":" << (node->red() ? "r" : "b");
}

void DBImpl::print_path(std::ostream& out, std::deque<SharedNodeRef>& path)
{
  out << "path: ";
  if (path.empty()) {
    out << "<empty>";
  } else {
    out << "[";
    for (auto node : path) {
      if (node == Node::Nil())
        out << "nil:" << (node->red() ? "r " : "b ");
      else
        out << node->key().ToString() << ":" << (node->red() ? "r " : "b ");
    }
    out << "]";
  }
  out << std::endl;
}

/*
 *
 */
int DBImpl::_validate_rb_tree(const SharedNodeRef root)
{
  assert(root != nullptr);

  assert(root->read_only());
  if (!root->read_only())
    return 0;

  if (root == Node::Nil())
    return 1;

  assert(root->left.ref_notrace());
  assert(root->right.ref_notrace());

  SharedNodeRef ln = root->left.ref_notrace();
  SharedNodeRef rn = root->right.ref_notrace();

  if (root->red() && (ln->red() || rn->red()))
    return 0;

  int lh = _validate_rb_tree(ln);
  int rh = _validate_rb_tree(rn);

  if ((ln != Node::Nil() && ln->key().compare(root->key()) >= 0) ||
      (rn != Node::Nil() && rn->key().compare(root->key()) <= 0))
    return 0;

  if (lh != 0 && rh != 0 && lh != rh)
    return 0;

  if (lh != 0 && rh != 0)
    return root->red() ? lh : lh + 1;

  return 0;
}

void DBImpl::validate_rb_tree(NodePtr root)
{
  assert(_validate_rb_tree(root.ref_notrace()) != 0);
}

Transaction *DBImpl::BeginTransaction()
{
  std::lock_guard<std::mutex> l(lock_);
  auto txn = new TransactionImpl(this, root_, root_pos_, root_id_++);
  // FIXME: this is a temporary check; we currently do not have any tests or
  // benchmarks that are multi-threaded. soon we will actually block new
  // transactions from starting until the current txn finishes.
  assert(!cur_txn_);
  cur_txn_ = txn;
  return txn;
}

void DBImpl::TransactionFinisher()
{
  while (true) {
    std::unique_lock<std::mutex> lk(lock_);

    if (stop_)
      return;

    if (!cur_txn_ || !cur_txn_->Committed()) {
      txn_finisher_cond_.wait(lk);
      continue;
    }

    // serialize the transaction after image
    std::string blob;
    cur_txn_->SerializeAfterImage(&blob);

    // append after image to the log
    uint64_t pos;
    int ret = log_->Append(blob, &pos);
    assert(ret == 0);

    // deserialize after image
    kvstore_proto::Intention i;
    assert(i.ParseFromString(blob));
    assert(i.IsInitialized());

    // update root ptr with new position etc...
    auto root = cache_.CacheIntention(i, pos);
    root_.replace(root);
    root_pos_ = pos;

    root_desc_.clear();
    for (int idx = 0; idx < i.description_size(); idx++)
      root_desc_.push_back(i.description(idx));

    // mark complete
    cur_txn_->MarkComplete();
    cur_txn_ = nullptr;

    // optimizations:
    //   1. add intention to cache rather than waiting on cache miss
    //   2. better than (1): fold txn in-memory repr. into cache
  }
}

void DBImpl::AbortTransaction(TransactionImpl *txn)
{
  assert(txn == cur_txn_);
  assert(!txn->Committed());
  assert(!txn->Completed());

  std::lock_guard<std::mutex> lk(lock_);
  cur_txn_ = nullptr;
}

void DBImpl::CompleteTransaction(TransactionImpl *txn)
{
  assert(txn == cur_txn_);
  assert(txn->Committed());
  assert(!txn->Completed());

  // notify txn finisher
  txn_finisher_cond_.notify_one();
}
