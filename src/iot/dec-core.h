// iot/dec-core.h

#ifndef KALDI_IOT_DEC_CORE_H_
#define KALDI_IOT_DEC_CORE_H_

#include "util/stl-utils.h"
#include "util/hash-list.h"
#include "fst/fstlib.h"
#include "itf/decodable-itf.h"
#include "fstext/fstext-lib.h"
#include "lat/determinize-lattice-pruned.h"
#include "lat/kaldi-lattice.h"

#include "iot/wfst.h"

namespace kaldi {
namespace iot {

struct DecCoreConfig {
  BaseFloat beam;
  int32 max_active;
  int32 min_active;
  BaseFloat lattice_beam;
  int32 prune_interval;
  bool determinize_lattice; // not inspected by this class... used in
                            // command-line program.
  BaseFloat beam_delta; // has nothing to do with beam_ratio
  BaseFloat hash_ratio;
  BaseFloat prune_scale;   // Note: we don't make this configurable on the command line,
                           // it's not a very important parameter.  It affects the
                           // algorithm that prunes the tokens as we go.
  // Most of the options inside det_opts are not actually queried by the
  // LatticeFasterDecoder class itself, but by the code that calls it, for
  // example in the function DecodeUtteranceLatticeFaster.
  fst::DeterminizeLatticePhonePrunedOptions det_opts;

  DecCoreConfig(): 
    beam(16.0),
    max_active(std::numeric_limits<int32>::max()),
    min_active(200),
    lattice_beam(10.0),
    prune_interval(25),
    determinize_lattice(true),
    beam_delta(0.5),
    hash_ratio(2.0),
    prune_scale(0.1) 
  { }

  void Register(OptionsItf *opts) {
    det_opts.Register(opts);
    opts->Register("beam", &beam, "Decoding beam.  Larger->slower, more accurate.");
    opts->Register("max-active", &max_active, "Decoder max active states.  Larger->slower; "
                   "more accurate");
    opts->Register("min-active", &min_active, "Decoder minimum #active states.");
    opts->Register("lattice-beam", &lattice_beam, "Lattice generation beam.  Larger->slower, "
                   "and deeper lattices");
    opts->Register("prune-interval", &prune_interval, "Interval (in frames) at "
                   "which to prune tokens");
    opts->Register("determinize-lattice", &determinize_lattice, "If true, "
                   "determinize the lattice (lattice-determinization, keeping only "
                   "best pdf-sequence for each word-sequence).");
    opts->Register("beam-delta", &beam_delta, "Increment used in decoding-- this "
                   "parameter is obscure and relates to a speedup in the way the "
                   "max-active constraint is applied.  Larger is more accurate.");
    opts->Register("hash-ratio", &hash_ratio, "Setting used in decoder to "
                   "control hash behavior");
  }
  void Check() const {
    KALDI_ASSERT(beam > 0.0 && max_active > 1 && lattice_beam > 0.0
                 && prune_interval > 0 && beam_delta > 0.0 && hash_ratio >= 1.0
                 && prune_scale > 0.0 && prune_scale < 1.0);
  }
};


class DecCore {
 public:
  typedef fst::StdArc Arc;
  typedef Arc::Label Label;
  typedef Arc::StateId StateId;
  typedef Arc::Weight Weight;

  struct BestPathIterator {
    void *tok;
    int32 frame;
    // note, "frame" is the frame-index of the frame you'll get the
    // transition-id for next time, if you call TraceBackBestPath on this
    // iterator (assuming it's not an epsilon transition).  Note that this
    // is one less than you might reasonably expect, e.g. it's -1 for
    // the nonemitting transitions before the first frame.
    BestPathIterator(void *t, int32 f): tok(t), frame(f) { }
    bool Done() { return tok == NULL; }
  };

  DecCore(Wfst *fst, const DecCoreConfig &config);
  ~DecCore();

  void SetOptions(const DecCoreConfig &config) { config_ = config; }
  const DecCoreConfig &GetOptions() const { return config_; }

  bool Decode(DecodableInterface *decodable);

  void InitDecoding();
  void AdvanceDecoding(DecodableInterface *decodable, int32 max_num_frames = -1);
  void FinalizeDecoding();

  inline int32 NumFramesDecoded() const { return active_toks_.size() - 1; }
  BaseFloat FinalRelativeCost() const;

  bool ReachedFinal() const {
    return FinalRelativeCost() != std::numeric_limits<BaseFloat>::infinity();
  }

  BestPathIterator BestPathEnd(bool use_final_probs, BaseFloat *final_cost = NULL) const;
  BestPathIterator TraceBackBestPath(BestPathIterator iter, LatticeArc *arc) const;
  bool GetBestPath(Lattice *ofst, bool use_final_probs = true) const;
  bool TestGetBestPath(bool use_final_probs = true) const;

  bool GetRawLattice(Lattice *ofst, bool use_final_probs = true) const;
  bool GetRawLatticePruned(Lattice *ofst, bool use_final_probs, BaseFloat beam) const;

 private:

  struct Token;

  struct ForwardLink {
    Token *next_tok; // the next token [or NULL if represents final-state]
    Label ilabel;
    Label olabel;
    BaseFloat graph_cost; // graph cost of traversing link (contains LM, etc.)
    BaseFloat acoustic_cost; // acoustic cost (pre-scaled) of traversing link
    ForwardLink *next;

    inline ForwardLink(Token *next_tok,
                       Label ilabel,
                       Label olabel,
                       BaseFloat graph_cost,
                       BaseFloat acoustic_cost,
                       ForwardLink *next) :
      next_tok(next_tok),
      ilabel(ilabel),
      olabel(olabel),
      graph_cost(graph_cost),
      acoustic_cost(acoustic_cost),
      next(next)
    { }
  };

  struct Token {
    BaseFloat total_cost;
    BaseFloat extra_cost;
    ForwardLink *links;
    Token *next;
    Token *backpointer;

    inline Token(BaseFloat total_cost,
                 BaseFloat extra_cost,
                 ForwardLink *links,
                 Token *next,
                 Token *backpointer) :
      total_cost(total_cost),
      extra_cost(extra_cost),
      links(links),
      next(next),
      backpointer(backpointer) 
    { }

    inline void DeleteForwardLinks() {
      ForwardLink *l = links, *m;
      while (l != NULL) {
        m = l->next;
        delete l;
        l = m;
      }
      links = NULL;
    }
  };

  struct TokenList {
    Token *toks;
    bool must_prune_forward_links;
    bool must_prune_tokens;

    TokenList() :
      toks(NULL),
      must_prune_forward_links(true),
      must_prune_tokens(true)
    { }
  };

  typedef HashList<StateId, Token*>::Elem Elem;

  void PossiblyResizeHash(size_t num_toks);

  // FindOrAddToken either locates a token in hash of toks_, or if necessary
  // inserts a new, empty token (i.e. with no forward links) for the current
  // frame.  [note: it's inserted if necessary into hash toks_ and also into the
  // singly linked list of tokens active on this frame (whose head is at
  // active_toks_[frame]).  The frame_plus_one argument is the acoustic frame
  // index plus one, which is used to index into the active_toks_ array.
  // Returns the Token pointer.  Sets "changed" (if non-NULL) to true if the
  // token was newly created or the cost changed.
  inline Token *FindOrAddToken(StateId state, int32 frame_plus_one,
                               BaseFloat total_cost, Token *backpointer,
                               bool *changed);

  // prunes outgoing links for all tokens in active_toks_[frame]
  // it's called by PruneActiveTokens
  // all links, that have link_extra_cost > lattice_beam are pruned
  // delta is the amount by which the extra_costs must change
  // before we set *extra_costs_changed = true.
  // If delta is larger,  we'll tend to go back less far
  //    toward the beginning of the file.
  // extra_costs_changed is set to true if extra_cost was changed for any token
  // links_pruned is set to true if any link in any token was pruned
  void PruneForwardLinks(int32 frame_plus_one, bool *extra_costs_changed,
                         bool *links_pruned,
                         BaseFloat delta);

  // This function computes the final-costs for tokens active on the final
  // frame.  It outputs to final-costs, if non-NULL, a map from the Token*
  // pointer to the final-prob of the corresponding state, for all Tokens
  // that correspond to states that have final-probs.  This map will be
  // empty if there were no final-probs.  It outputs to
  // final_relative_cost, if non-NULL, the difference between the best
  // forward-cost including the final-prob cost, and the best forward-cost
  // without including the final-prob cost (this will usually be positive), or
  // infinity if there were no final-probs.  [c.f. FinalRelativeCost(), which
  // outputs this quanitity].  It outputs to final_best_cost, if
  // non-NULL, the lowest for any token t active on the final frame, of
  // forward-cost[t] + final-cost[t], where final-cost[t] is the final-cost in
  // the graph of the state corresponding to token t, or the best of
  // forward-cost[t] if there were no final-probs active on the final frame.
  // You cannot call this after FinalizeDecoding() has been called; in that
  // case you should get the answer from class-member variables.
  void ComputeFinalCosts(unordered_map<Token*, BaseFloat> *final_costs,
                         BaseFloat *final_relative_cost,
                         BaseFloat *final_best_cost) const;

  // PruneForwardLinksFinal is a version of PruneForwardLinks that we call
  // on the final frame.  If there are final tokens active, it uses
  // the final-probs for pruning, otherwise it treats all tokens as final.
  void PruneForwardLinksFinal();

  // Prune away any tokens on this frame that have no forward links.
  // [we don't do this in PruneForwardLinks because it would give us
  // a problem with dangling pointers].
  // It's called by PruneActiveTokens if any forward links have been pruned
  void PruneTokensForFrame(int32 frame_plus_one);


  // Go backwards through still-alive tokens, pruning them if the
  // forward+backward cost is more than lat_beam away from the best path.  It's
  // possible to prove that this is "correct" in the sense that we won't lose
  // anything outside of lat_beam, regardless of what happens in the future.
  // delta controls when it considers a cost to have changed enough to continue
  // going backward and propagating the change.  larger delta -> will recurse
  // less far.
  void PruneActiveTokens(BaseFloat delta);

  /// Gets the weight cutoff.  Also counts the active tokens.
  BaseFloat GetCutoff(Elem *list_head, size_t *tok_count,
                      BaseFloat *adaptive_beam, Elem **best_elem);

  BaseFloat ProcessEmitting(DecodableInterface *decodable);
  void ProcessNonemitting(BaseFloat cost_cutoff);

  // HashList defined in ../util/hash-list.h.  It actually allows us to maintain
  // more than one list (e.g. for current and previous frames), but only one of
  // them at a time can be indexed by StateId.  It is indexed by frame-index
  // plus one, where the frame-index is zero-based, as used in decodable object.
  // That is, the emitting probs of frame t are accounted for in tokens at
  // toks_[t+1].  The zeroth frame is for nonemitting transition at the start of
  // the graph.
  HashList<StateId, Token*> toks_;

  std::vector<TokenList> active_toks_; // Lists of tokens, indexed by
  // frame (members of TokenList are toks, must_prune_forward_links,
  // must_prune_tokens).

  std::vector<StateId> queue_;
  std::vector<BaseFloat> tmp_array_;

  Wfst *fst_;

  std::vector<BaseFloat> cost_offsets_;
  DecCoreConfig config_;
  int32 num_toks_; // current total #toks allocated...
  bool warned_;

  /// decoding_finalized_ is true if someone called FinalizeDecoding().  [note,
  /// calling this is optional].  If true, it's forbidden to decode more.  Also,
  /// if this is set, then the output of ComputeFinalCosts() is in the next
  /// three variables.  The reason we need to do this is that after
  /// FinalizeDecoding() calls PruneTokensForFrame() for the final frame, some
  /// of the tokens on the last frame are freed, so we free the list from toks_
  /// to avoid having dangling pointers hanging around.
  bool decoding_finalized_;
  /// For the meaning of the next 3 variables, see the comment for
  /// decoding_finalized_ above., and ComputeFinalCosts().
  unordered_map<Token*, BaseFloat> final_costs_;
  BaseFloat final_relative_cost_;
  BaseFloat final_best_cost_;

  // There are various cleanup tasks... the the toks_ structure contains
  // singly linked lists of Token pointers, where Elem is the list type.
  // It also indexes them in a hash, indexed by state (this hash is only
  // maintained for the most recent frame).  toks_.Clear()
  // deletes them from the hash and returns the list of Elems.  The
  // function DeleteElems calls toks_.Delete(elem) for each elem in
  // the list, which returns ownership of the Elem to the toks_ structure
  // for reuse, but does not delete the Token pointer.  The Token pointers
  // are reference-counted and are ultimately deleted in PruneTokensForFrame,
  // but are also linked together on each frame by their own linked-list,
  // using the "next" pointer.  We delete them manually.
  void DeleteElems(Elem *list);

  // This function takes a singly linked list of tokens for a single frame, and
  // outputs a list of them in topological order (it will crash if no such order
  // can be found, which will typically be due to decoding graphs with epsilon
  // cycles, which are not allowed).  Note: the output list may contain NULLs,
  // which the caller should pass over; it just happens to be more efficient for
  // the algorithm to output a list that contains NULLs.
  static void TopSortTokens(Token *tok_list, std::vector<Token*> *topsorted_list);

  void ClearActiveTokens();
  
  KALDI_DISALLOW_COPY_AND_ASSIGN(DecCore);
};


} // end namespace iot
} // end namespace kaldi.
#endif
