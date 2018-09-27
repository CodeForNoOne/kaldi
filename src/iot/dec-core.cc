#include "iot/dec-core.h"

namespace kaldi {
namespace iot {

DecCore::DecCore(Wfst *la_fst, 
                 fst::DeterministicOnDemandFst<fst::StdArc> *lm_fst,
                 const TransitionModel &trans_model, 
                 const DecCoreConfig &config)
  : la_fst_(la_fst),
    lm_fst_(lm_fst),
    trans_model_(trans_model), 
    config_(config)
{
  config_.Check();
  token_set_.SetSize(1000);
  num_toks_ = 0; 

  token_pool_ = new MemoryPool(sizeof(Token), config_.token_pool_realloc);
  link_pool_  = new MemoryPool(sizeof(ForwardLink), config_.link_pool_realloc);
}

DecCore::~DecCore() {
  DeleteElems(token_set_.Clear());
  ClearTokenNet();

  DELETE(token_pool_);
  DELETE(link_pool_);
}

void DecCore::InitDecoding() {
  // clean up from last time:
  DeleteElems(token_set_.Clear());
  cost_offsets_.clear();
  ClearTokenNet();
  warned_ = false;
  num_toks_ = 0;
  decoding_finalized_ = false;
  final_costs_.clear();

  WfstStateId la_start_state = la_fst_->Start();
  WfstStateId lm_start_state = (lm_fst_ == NULL) ? 0 : lm_fst_->Start();
  ViterbiState start_state = ComposeViterbiState(la_start_state, lm_start_state);

  Token *start_token = NewToken(0.0, 0.0, NULL, NULL, NULL);
  num_toks_++;

  token_net_.resize(1);
  token_net_[0].toks = start_token;

  token_set_.Insert(start_state, start_token);

  ProcessNonemitting(config_.beam);
}

// Returns true if any kind of traceback is available (not necessarily from
// a final state).  It should only very rarely return false; this indicates
// an unusual search error.
bool DecCore::Decode(DecodableInterface *decodable) {
  InitDecoding();

  while (!decodable->IsLastFrame(NumFramesDecoded() - 1)) {
    if (NumFramesDecoded() % config_.prune_interval == 0) {
      PruneTokenNet(config_.lattice_beam * config_.prune_scale);
    }

    BaseFloat cost_cutoff = ProcessEmitting(decodable);
    ProcessNonemitting(cost_cutoff);
  }
  FinalizeDecoding();

  return !token_net_.empty() && token_net_.back().toks != NULL;
}


void DecCore::AdvanceDecoding(DecodableInterface *decodable, int32 max_num_frames) {
  KALDI_ASSERT(!token_net_.empty() && !decoding_finalized_ &&
               "You must call InitDecoding() before AdvanceDecoding");
  int32 num_frames_ready = decodable->NumFramesReady();
  KALDI_ASSERT(num_frames_ready >= NumFramesDecoded());

  int32 target_frames_decoded = num_frames_ready;
  if (max_num_frames >= 0) {
    target_frames_decoded = std::min(target_frames_decoded,
                                     NumFramesDecoded() + max_num_frames);
  }

  while (NumFramesDecoded() < target_frames_decoded) {
    if (NumFramesDecoded() % config_.prune_interval == 0) {
      PruneTokenNet(config_.lattice_beam * config_.prune_scale);
    }
    BaseFloat cost_cutoff = ProcessEmitting(decodable);
    ProcessNonemitting(cost_cutoff);
  }
}


// FinalizeDecoding() is a version of PruneTokenNet that we call
// (optionally) on the final frame.  Takes into account the final-prob of
// tokens.  This function used to be called PruneTokenNetFinal().
void DecCore::FinalizeDecoding() {
  int32 end_time = NumFramesDecoded();
  int32 num_toks_begin = num_toks_;
  // PruneForwardLinksFinal() prunes final frame (with final-probs), and
  // sets decoding_finalized_.
  PruneForwardLinksFinal();
  for (int32 t = end_time - 1; t >= 0; t--) {
    bool b1, b2; // values not used.
    BaseFloat dontcare = 0.0; // delta of zero means we must always update
    PruneForwardLinks(t, &b1, &b2, dontcare);
    PruneTokenList(t + 1);
  }
  PruneTokenList(0);
  KALDI_VLOG(4) << "pruned tokens from " << num_toks_begin
                << " to " << num_toks_;
}


int32 DecCore::TrailingSilenceFrames() const {
  bool use_final_probs = false;
  DecCore::BestPathIterator iter = BestPathEnd(use_final_probs, NULL);
  int32 trailing_silence_frames = 0;
  while (!iter.Done()) {
    LatticeArc arc;
    iter = TraceBackBestPath(iter, &arc);
    if (arc.ilabel != kWfstEpsilon) {
      int32 phone_id = trans_model_.TransitionIdToPhone(arc.ilabel);
      if (phone_id == kSilencePhoneId) {
        trailing_silence_frames++;
      } else {
        break; // stop counting as soon as we hit non-silence.
      }
    }
  }
  return trailing_silence_frames;
}


BaseFloat DecCore::FinalRelativeCost() const {
  if (!decoding_finalized_) {
    BaseFloat relative_cost;
    ComputeFinalCosts(NULL, &relative_cost, NULL);
    return relative_cost;
  } else {
    // we're not allowed to call that function if FinalizeDecoding() has
    // been called; return a cached value.
    return final_relative_cost_;
  }
}


void DecCore::ComputeFinalCosts(
    unordered_map<Token*, BaseFloat> *final_costs,
    BaseFloat *final_relative_cost,
    BaseFloat *final_best_cost) const {
  KALDI_ASSERT(!decoding_finalized_);
  if (final_costs != NULL) {
    final_costs->clear();
  }

  BaseFloat infinity = std::numeric_limits<BaseFloat>::infinity();
  BaseFloat best_cost = infinity,
            best_cost_with_final = infinity;

  const Elem *e = token_set_.GetList();
  while (e != NULL) {
    ViterbiState state = e->key;
    WfstStateId la_state = ExtractLaState(state),
                lm_state = ExtractLmState(state);

    Token *tok = e->val;

    BaseFloat la_final_cost = la_fst_->Final(la_state);
    BaseFloat lm_final_cost = (lm_fst_ == NULL) ? 0.0f : lm_fst_->Final(lm_state).Value(); //TODO: validate if 0.0f is appropriate
    BaseFloat final_cost = la_final_cost + lm_final_cost;
    
    BaseFloat cost = tok->total_cost,
              cost_with_final = cost + final_cost;
    best_cost = std::min(cost, best_cost);
    best_cost_with_final = std::min(cost_with_final, best_cost_with_final);

    if (final_costs != NULL && final_cost != infinity)
      (*final_costs)[tok] = final_cost;
      
    e = e->tail;
  }

  if (final_relative_cost != NULL) {
    if (best_cost == infinity && best_cost_with_final == infinity) {
      // Likely this will only happen if there are no tokens surviving.
      // This seems the least bad way to handle it.
      *final_relative_cost = infinity;
    } else {
      *final_relative_cost = best_cost_with_final - best_cost;
    }
  }

  if (final_best_cost != NULL) {
    if (best_cost_with_final != infinity) { // final-state exists.
      *final_best_cost = best_cost_with_final;
    } else { // no final-state exists.
      *final_best_cost = best_cost;
    }
  }
}


DecCore::BestPathIterator DecCore::BestPathEnd(bool use_final_probs, 
                                               BaseFloat *final_cost_out) const {
  if (decoding_finalized_ && !use_final_probs)
    KALDI_ERR << "You cannot call FinalizeDecoding() and then call "
              << "BestPathEnd() with use_final_probs == false";
  KALDI_ASSERT(NumFramesDecoded() > 0 &&
               "You cannot call BestPathEnd if no frames were decoded.");
  
  unordered_map<Token*, BaseFloat> final_costs_local;

  const unordered_map<Token*, BaseFloat> &final_costs =
      (decoding_finalized_ ? final_costs_ : final_costs_local);
  if (!decoding_finalized_ && use_final_probs) {
    ComputeFinalCosts(&final_costs_local, NULL, NULL);
  }
  
  // Singly linked list of tokens on last frame (access list through "next" pointer).
  BaseFloat best_cost = std::numeric_limits<BaseFloat>::infinity();
  BaseFloat best_final_cost = 0;
  Token *best_tok = NULL;
  for (Token *tok = token_net_.back().toks; tok != NULL; tok = tok->next) {
    BaseFloat cost = tok->total_cost, final_cost = 0.0;
    if (use_final_probs && !final_costs.empty()) {
      // if we are instructed to use final-probs, and any final tokens were
      // active on final frame, include the final-prob in the cost of the token.
      unordered_map<Token*, BaseFloat>::const_iterator iter = final_costs.find(tok);
      if (iter != final_costs.end()) {
        final_cost = iter->second;
        cost += final_cost;
      } else {
        cost = std::numeric_limits<BaseFloat>::infinity();
      }
    }
    if (cost < best_cost) {
      best_cost = cost;
      best_tok = tok;
      best_final_cost = final_cost;
    }
  }    
  if (best_tok == NULL) {  // this should not happen, and is likely a code error or
    // caused by infinities in likelihoods, but I'm not making
    // it a fatal error for now.
    KALDI_WARN << "No final token found.";
  }
  if (final_cost_out)
    *final_cost_out = best_final_cost;
  return BestPathIterator(best_tok, NumFramesDecoded() - 1);
}


DecCore::BestPathIterator DecCore::TraceBackBestPath(
    BestPathIterator iter, LatticeArc *oarc) const {
  KALDI_ASSERT(!iter.Done() && oarc != NULL);
  Token *tok = static_cast<Token*>(iter.tok);
  int32 cur_t = iter.frame, ret_t = cur_t;
  if (tok->backpointer != NULL) {
    ForwardLink *link;
    for (link = tok->backpointer->links; link != NULL; link = link->next) {
      if (link->dst_tok == tok) { // this is the link to "tok"
        oarc->ilabel = link->ilabel;
        oarc->olabel = link->olabel;
        BaseFloat graph_cost = link->graph_cost,
                  acoustic_cost = link->acoustic_cost;
        if (link->ilabel != 0) {
          KALDI_ASSERT(static_cast<size_t>(cur_t) < cost_offsets_.size());
          acoustic_cost -= cost_offsets_[cur_t];
          ret_t--;
        }
        oarc->weight = LatticeWeight(graph_cost, acoustic_cost);
        break;
      }
    }
    if (link == NULL) { // Did not find correct link.
      KALDI_ERR << "Error tracing best-path back (likely "
                << "bug in token-pruning algorithm)";
    }
  } else {
    oarc->ilabel = 0;
    oarc->olabel = 0;
    oarc->weight = LatticeWeight::One(); // zero costs.
  }
  return BestPathIterator(tok->backpointer, ret_t);
}


// Outputs an FST corresponding to the single best path through the lattice.
bool DecCore::GetBestPath(Lattice *olat, bool use_final_probs) const {
  olat->DeleteStates();
  BaseFloat final_graph_cost;
  BestPathIterator iter = BestPathEnd(use_final_probs, &final_graph_cost);
  if (iter.Done())
    return false;  // would have printed warning.
  StateId state = olat->AddState();
  olat->SetFinal(state, LatticeWeight(final_graph_cost, 0.0));
  while (!iter.Done()) {
    LatticeArc arc;
    iter = TraceBackBestPath(iter, &arc);
    arc.nextstate = state;
    StateId new_state = olat->AddState();
    olat->AddArc(new_state, arc);
    state = new_state;
  }
  olat->SetStart(state);
  return true;
}


bool DecCore::TestGetBestPath(bool use_final_probs) const {
  Lattice lat1;
  {
    Lattice raw_lat;
    GetRawLattice(&raw_lat, use_final_probs);
    ShortestPath(raw_lat, &lat1);
  }
  Lattice lat2;
  GetBestPath(&lat2, use_final_probs);  
  BaseFloat delta = 0.1;
  int32 num_paths = 1;
  if (!fst::RandEquivalent(lat1, lat2, num_paths, delta, rand())) {
    KALDI_WARN << "Best-path test failed";
    return false;
  } else {
    return true;
  }
}


// Outputs an FST corresponding to the raw, state-level tracebacks.
bool DecCore::GetRawLattice(Lattice *ofst, bool use_final_probs) const {
  typedef LatticeArc Arc;
  typedef Arc::StateId StateId;
  typedef Arc::Weight Weight;
  typedef Arc::Label Label;

  // Note: you can't use the old interface (Decode()) if you want to
  // get the lattice with use_final_probs = false.  You'd have to do
  // InitDecoding() and then AdvanceDecoding().
  if (decoding_finalized_ && !use_final_probs)
    KALDI_ERR << "You cannot call FinalizeDecoding() and then call "
              << "GetRawLattice() with use_final_probs == false";

  unordered_map<Token*, BaseFloat> final_costs_local;

  const unordered_map<Token*, BaseFloat> &final_costs =
      (decoding_finalized_ ? final_costs_ : final_costs_local);
  if (!decoding_finalized_ && use_final_probs)
    ComputeFinalCosts(&final_costs_local, NULL, NULL);

  ofst->DeleteStates();
  // num-frames plus one (since frames are one-based, and we have
  // an extra frame for the start-state).
  int32 num_frames = token_net_.size() - 1;
  KALDI_ASSERT(num_frames > 0);
  const int32 bucket_count = num_toks_/2 + 3;
  unordered_map<Token*, StateId> tok_map(bucket_count);
  // First create all states.
  std::vector<Token*> token_list;
  for (int32 f = 0; f <= num_frames; f++) {
    if (token_net_[f].toks == NULL) {
      KALDI_WARN << "GetRawLattice: no tokens active on frame " << f
                 << ": not producing lattice.\n";
      return false;
    }
    TopSortTokens(token_net_[f].toks, &token_list);
    for (size_t i = 0; i < token_list.size(); i++)
      if (token_list[i] != NULL)
        tok_map[token_list[i]] = ofst->AddState();    
  }
  // The next statement sets the start state of the output FST.  Because we
  // topologically sorted the tokens, state zero must be the start-state.
  ofst->SetStart(0);
  
  KALDI_VLOG(4) << "init:" << num_toks_/2 + 3 << " buckets:"
                << tok_map.bucket_count() << " load:" << tok_map.load_factor()
                << " max:" << tok_map.max_load_factor();
  // Now create all arcs.
  for (int32 f = 0; f <= num_frames; f++) {
    for (Token *tok = token_net_[f].toks; tok != NULL; tok = tok->next) {
      StateId cur_state = tok_map[tok];
      for (ForwardLink *l = tok->links; l != NULL; l = l->next) {
        unordered_map<Token*, StateId>::const_iterator iter =
            tok_map.find(l->dst_tok);
        StateId nextstate = iter->second;
        KALDI_ASSERT(iter != tok_map.end());
        BaseFloat cost_offset = 0.0;
        if (l->ilabel != 0) {  // emitting..
          KALDI_ASSERT(f >= 0 && f < cost_offsets_.size());
          cost_offset = cost_offsets_[f];
        }
        Arc arc(l->ilabel, l->olabel,
                Weight(l->graph_cost, l->acoustic_cost - cost_offset),
                nextstate);
        ofst->AddArc(cur_state, arc);
      }
      if (f == num_frames) {
        if (use_final_probs && !final_costs.empty()) {
          unordered_map<Token*, BaseFloat>::const_iterator iter =
              final_costs.find(tok);
          if (iter != final_costs.end())
            ofst->SetFinal(cur_state, LatticeWeight(iter->second, 0));
        } else {
          ofst->SetFinal(cur_state, LatticeWeight::One());
        }
      }
    }
  }
  return (ofst->NumStates() > 0);
}


bool DecCore::GetRawLatticePruned(
    Lattice *ofst,
    bool use_final_probs,
    BaseFloat beam) const {
  typedef LatticeArc Arc;
  typedef Arc::StateId StateId;
  typedef Arc::Weight Weight;
  typedef Arc::Label Label;

  // Note: you can't use the old interface (Decode()) if you want to
  // get the lattice with use_final_probs = false.  You'd have to do
  // InitDecoding() and then AdvanceDecoding().
  if (decoding_finalized_ && !use_final_probs)
    KALDI_ERR << "You cannot call FinalizeDecoding() and then call "
              << "GetRawLattice() with use_final_probs == false";

  unordered_map<Token*, BaseFloat> final_costs_local;

  const unordered_map<Token*, BaseFloat> &final_costs =
      (decoding_finalized_ ? final_costs_ : final_costs_local);
  if (!decoding_finalized_ && use_final_probs)
    ComputeFinalCosts(&final_costs_local, NULL, NULL);

  ofst->DeleteStates();
  // num-frames plus one (since frames are one-based, and we have
  // an extra frame for the start-state).
  int32 num_frames = token_net_.size() - 1;
  KALDI_ASSERT(num_frames > 0);
  for (int32 f = 0; f <= num_frames; f++) {
    if (token_net_[f].toks == NULL) {
      KALDI_WARN << "GetRawLattice: no tokens active on frame " << f
                 << ": not producing lattice.\n";
      return false;
    }
  }

  unordered_map<Token*, StateId> tok_map;
  std::queue<std::pair<Token*, int32> > tok_queue;
  // First initialize the queue and states.  Put the initial state on the queue;
  // this is the last token in the list token_net_[0].toks.
  for (Token *tok = token_net_[0].toks; tok != NULL; tok = tok->next) {
    if (tok->next == NULL) {
      tok_map[tok] = ofst->AddState();
      ofst->SetStart(tok_map[tok]);
      std::pair<Token*, int32> tok_pair(tok, 0);  // #frame = 0
      tok_queue.push(tok_pair);
    }
  }  
  
  // Next create states for "good" tokens
  while (!tok_queue.empty()) {
    std::pair<Token*, int32> cur_tok_pair = tok_queue.front();
    tok_queue.pop();
    Token *cur_tok = cur_tok_pair.first;
    int32 cur_frame = cur_tok_pair.second;
    KALDI_ASSERT(cur_frame >= 0 && cur_frame <= cost_offsets_.size());
    
    unordered_map<Token*, StateId>::const_iterator iter =
        tok_map.find(cur_tok);
    KALDI_ASSERT(iter != tok_map.end());
    StateId cur_state = iter->second;

    for (ForwardLink *l = cur_tok->links; l != NULL; l = l->next) {
      Token *dst_tok = l->dst_tok;
      if (dst_tok->extra_cost < beam) {
        // so both the current and the next token are good; create the arc
        int32 next_frame = l->ilabel == 0 ? cur_frame : cur_frame + 1;
        StateId nextstate;
        if (tok_map.find(dst_tok) == tok_map.end()) {
          nextstate = tok_map[dst_tok] = ofst->AddState();
          tok_queue.push(std::pair<Token*, int32>(dst_tok, next_frame));
        } else {
          nextstate = tok_map[dst_tok];
        }
        BaseFloat cost_offset = (l->ilabel != 0 ? cost_offsets_[cur_frame] : 0);
        Arc arc(l->ilabel, l->olabel,
                Weight(l->graph_cost, l->acoustic_cost - cost_offset),
                nextstate);
        ofst->AddArc(cur_state, arc);
      }
    }
    if (cur_frame == num_frames) {
      if (use_final_probs && !final_costs.empty()) {
        unordered_map<Token*, BaseFloat>::const_iterator iter =
            final_costs.find(cur_tok);
        if (iter != final_costs.end())
          ofst->SetFinal(cur_state, LatticeWeight(iter->second, 0));
      } else {        
        ofst->SetFinal(cur_state, LatticeWeight::One());
      }
    }
  }
  return (ofst->NumStates() != 0);
}


void DecCore::PossiblyResizeHash(size_t num_toks) {
  size_t new_size = static_cast<size_t>(static_cast<BaseFloat>(num_toks)
                                      * config_.hash_ratio);
  if (new_size > token_set_.Size()) {
    token_set_.SetSize(new_size);
  }
}


inline DecCore::Token *DecCore::FindOrAddToken(ViterbiState state, 
                                               int32 t, 
                                               BaseFloat total_cost,
                                               Token *backpointer, 
                                               bool *changed) {
  KALDI_ASSERT(t < token_net_.size());
  Token *&toks = token_net_[t].toks;
  Elem *e_found = token_set_.Find(state);
  if (e_found == NULL) {
    const BaseFloat extra_cost = 0.0;
    // tokens on the currently final frame have zero extra_cost
    // as any of them could end up on the winning path.
    Token *new_tok = NewToken(total_cost, extra_cost, NULL, toks, backpointer);
    // NULL: no forward links yet
    toks = new_tok;
    num_toks_++;
    token_set_.Insert(state, new_tok);
    if (changed) *changed = true;
    return new_tok;
  } else {
    Token *tok = e_found->val;
    if (tok->total_cost > total_cost) {  // replace old token
      tok->total_cost = total_cost;
      tok->backpointer = backpointer;
      // we don't allocate a new token, the old stays linked in token_net_
      // we only replace the total_cost
      // in the current frame, there are no forward links (and no extra_cost)
      // only in ProcessNonemitting we have to delete forward links
      // in case we visit a state for the second time
      // those forward links, that lead to this replaced token before:
      // they remain and will hopefully be pruned later (PruneForwardLinks...)
      if (changed) *changed = true;
    } else {
      if (changed) *changed = false;
    }
    return tok;
  }
}


void DecCore::PruneForwardLinks(int32 t, 
                                bool *extra_costs_changed, 
                                bool *links_pruned, 
                                BaseFloat delta) {
  *extra_costs_changed = false;
  *links_pruned = false;
  KALDI_ASSERT(t >= 0 && t < token_net_.size());
  if (token_net_[t].toks == NULL) {  // empty list; should not happen.
    if (!warned_) {
      KALDI_WARN << "No tokens alive [doing pruning].. warning first "
          "time only for each utterance\n";
      warned_ = true;
    }
  }

  // We have to iterate until there is no more change, because the links
  // are not guaranteed to be in topological order.
  bool changed = true;  // difference new minus old extra cost >= delta ?
  while (changed) {
    changed = false;
    for (Token *tok = token_net_[t].toks; tok != NULL; tok = tok->next) {
      ForwardLink *link, *prev_link = NULL;
      // will recompute tok_extra_cost for tok.
      BaseFloat tok_extra_cost = std::numeric_limits<BaseFloat>::infinity();
      // tok_extra_cost is the best (min) of link_extra_cost of outgoing links
      for (link = tok->links; link != NULL; ) {
        // See if we need to excise this link...
        Token *dst_tok = link->dst_tok;
        BaseFloat link_extra_cost = dst_tok->extra_cost +
            ((tok->total_cost + link->acoustic_cost + link->graph_cost)
             - dst_tok->total_cost);  // difference in brackets is >= 0
        // link_exta_cost is the difference in score between the best paths
        // through link source state and through link destination state
        KALDI_ASSERT(link_extra_cost == link_extra_cost);  // check for NaN
        if (link_extra_cost > config_.lattice_beam) {  // excise link
          ForwardLink *next_link = link->next;
          if (prev_link != NULL) prev_link->next = next_link;
          else tok->links = next_link;
          DeleteLink(link);
          link = next_link;  // advance link but leave prev_link the same.
          *links_pruned = true;
        } else {   // keep the link and update the tok_extra_cost if needed.
          if (link_extra_cost < 0.0) {  // this is just a precaution.
            if (link_extra_cost < -0.01)
              KALDI_WARN << "Negative extra_cost: " << link_extra_cost;
            link_extra_cost = 0.0;
          }
          if (link_extra_cost < tok_extra_cost) {
            tok_extra_cost = link_extra_cost;
          }
          prev_link = link;  // move to next link
          link = link->next;
        }
      }  // for all outgoing links
      if (fabs(tok_extra_cost - tok->extra_cost) > delta) {
        changed = true;   // difference new minus old is bigger than delta
      }
      tok->extra_cost = tok_extra_cost;
      // will be +infinity or <= lattice_beam_.
      // infinity indicates, that no forward link survived pruning
    }  // for all Token on token_net_[frame]
    if (changed) *extra_costs_changed = true;

    // Note: it's theoretically possible that aggressive compiler
    // optimizations could cause an infinite loop here for small delta and
    // high-dynamic-range scores.
  } // while changed
}


void DecCore::PruneForwardLinksFinal() {
  KALDI_ASSERT(!token_net_.empty());
  int32 end_time = NumFramesDecoded();

  if (token_net_[end_time].toks == NULL ) {
    // empty list; should not happen.
    KALDI_WARN << "No tokens alive at end of file\n";
  }

  typedef unordered_map<Token*, BaseFloat>::const_iterator IterType;
  ComputeFinalCosts(&final_costs_, &final_relative_cost_, &final_best_cost_);
  decoding_finalized_ = true;
  // We call DeleteElems() as a nicety, not because it's really necessary;
  // otherwise there would be a time, after calling PruneTokenList() on the
  // final frame, when token_set_.GetList() or token_set_.Clear() would contain pointers
  // to nonexistent tokens.
  DeleteElems(token_set_.Clear());

  // Now go through tokens on this frame, pruning forward links...  may have to
  // iterate a few times until there is no more change, because the list is not
  // in topological order.  This is a modified version of the code in
  // PruneForwardLinks, but here we also take account of the final-probs.
  bool changed = true;
  BaseFloat delta = 1.0e-05;
  while (changed) {
    changed = false;
    for (Token *tok = token_net_[end_time].toks;  tok != NULL;  tok = tok->next) {
      ForwardLink *link, *prev_link = NULL;
      // will recompute tok_extra_cost.  It has a term in it that corresponds
      // to the "final-prob", so instead of initializing tok_extra_cost to infinity
      // below we set it to the difference between the (score+final_prob) of this token,
      // and the best such (score+final_prob).
      BaseFloat final_cost;
      if (final_costs_.empty()) {
        final_cost = 0.0;
      } else {
        IterType iter = final_costs_.find(tok);
        if (iter != final_costs_.end())
          final_cost = iter->second;
        else
          final_cost = std::numeric_limits<BaseFloat>::infinity();
      }
      BaseFloat tok_extra_cost = tok->total_cost + final_cost - final_best_cost_;
      // tok_extra_cost will be a "min" over either directly being final, or
      // being indirectly final through other links, and the loop below may
      // decrease its value:
      for (link = tok->links; link != NULL; ) {
        // See if we need to excise this link...
        Token *dst_tok = link->dst_tok;
        BaseFloat link_extra_cost = dst_tok->extra_cost +
            ((tok->total_cost + link->acoustic_cost + link->graph_cost)
             - dst_tok->total_cost);
        if (link_extra_cost > config_.lattice_beam) {  // excise link
          ForwardLink *next_link = link->next;
          if (prev_link != NULL) prev_link->next = next_link;
          else tok->links = next_link;
          DeleteLink(link);
          link = next_link; // advance link but leave prev_link the same.
        } else { // keep the link and update the tok_extra_cost if needed.
          if (link_extra_cost < 0.0) { // this is just a precaution.
            if (link_extra_cost < -0.01)
              KALDI_WARN << "Negative extra_cost: " << link_extra_cost;
            link_extra_cost = 0.0;
          }
          if (link_extra_cost < tok_extra_cost) {
            tok_extra_cost = link_extra_cost;
          }
          prev_link = link;
          link = link->next;
        }
      }
      // prune away tokens worse than lattice_beam above best path.  This step
      // was not necessary in the non-final case because then, this case
      // showed up as having no forward links.  Here, the tok_extra_cost has
      // an extra component relating to the final-prob.
      if (tok_extra_cost > config_.lattice_beam) {
        tok_extra_cost = std::numeric_limits<BaseFloat>::infinity();
      } // to be pruned in PruneTokenList

      if (!ApproxEqual(tok->extra_cost, tok_extra_cost, delta)) {
        changed = true;
      }
      tok->extra_cost = tok_extra_cost; // will be +infinity or <= lattice_beam_.
    }
  } // while changed
}


void DecCore::PruneTokenList(int32 t) {
  KALDI_ASSERT(t >= 0 && t < token_net_.size());
  Token *&toks = token_net_[t].toks;
  if (toks == NULL) {
    KALDI_WARN << "No tokens alive [doing pruning]\n";
  }

  Token *tok, *next_tok, *prev_tok = NULL;
  for (tok = toks; tok != NULL; tok = next_tok) {
    next_tok = tok->next;
    if (tok->extra_cost == std::numeric_limits<BaseFloat>::infinity()) {
      // token is unreachable from end of graph
      if (prev_tok != NULL) prev_tok->next = tok->next;
      else toks = tok->next;
      DeleteToken(tok);
      num_toks_--;
    } else {
      prev_tok = tok;
    }
  }
}


void DecCore::PruneTokenNet(BaseFloat delta) {
  int32 cur_time = NumFramesDecoded();
  int32 num_toks_begin = num_toks_;

  for (int32 t = cur_time - 1; t >= 0; t--) {
    if (token_net_[t].must_prune_forward_links) {
      bool extra_costs_changed = false, links_pruned = false;
      PruneForwardLinks(t, &extra_costs_changed, &links_pruned, delta);
      if (extra_costs_changed && t > 0) {
        token_net_[t-1].must_prune_forward_links = true;
      }
      if (links_pruned) {
        token_net_[t].must_prune_tokens = true;
      }
      token_net_[t].must_prune_forward_links = false;
    }
    if (t != cur_time - 1 && token_net_[t+1].must_prune_tokens) {
      PruneTokenList(t+1);
      token_net_[t+1].must_prune_tokens = false;
    }
  }
  KALDI_VLOG(4) << "PruneTokenNet: pruned tokens from " << num_toks_begin
                << " to " << num_toks_;
}


/// Gets the weight cutoff.  Also counts the active tokens.
BaseFloat DecCore::GetCutoff(Elem *list_head, 
                             size_t *tok_count,
                             BaseFloat *adaptive_beam, 
                             Elem **best_elem) {
  BaseFloat best_weight = std::numeric_limits<BaseFloat>::infinity();
  // positive == high cost == bad.
  size_t count = 0;
  if (config_.max_active == std::numeric_limits<int32>::max() &&
      config_.min_active == 0) {
    for (Elem *e = list_head; e != NULL; e = e->tail, count++) {
      BaseFloat w = static_cast<BaseFloat>(e->val->total_cost);
      if (w < best_weight) {
        best_weight = w;
        if (best_elem) *best_elem = e;
      }
    }
    if (tok_count != NULL) *tok_count = count;
    if (adaptive_beam != NULL) *adaptive_beam = config_.beam;
    return best_weight + config_.beam;
  } else {
    tmp_array_.clear();
    for (Elem *e = list_head; e != NULL; e = e->tail, count++) {
      BaseFloat w = e->val->total_cost;
      tmp_array_.push_back(w);
      if (w < best_weight) {
        best_weight = w;
        if (best_elem) *best_elem = e;
      }
    }
    if (tok_count != NULL) *tok_count = count;
    
    BaseFloat beam_cutoff = best_weight + config_.beam,
        min_active_cutoff = std::numeric_limits<BaseFloat>::infinity(),
        max_active_cutoff = std::numeric_limits<BaseFloat>::infinity();

    KALDI_VLOG(6) << "Number of tokens active on frame " << NumFramesDecoded()
                  << " is " << tmp_array_.size();

    if (tmp_array_.size() > static_cast<size_t>(config_.max_active)) {
      std::nth_element(tmp_array_.begin(),
                       tmp_array_.begin() + config_.max_active,
                       tmp_array_.end());
      max_active_cutoff = tmp_array_[config_.max_active];
    }
    if (max_active_cutoff < beam_cutoff) { // max_active is tighter than beam.
      if (adaptive_beam)
        *adaptive_beam = max_active_cutoff - best_weight + config_.beam_delta;
      return max_active_cutoff;
    }    
    if (tmp_array_.size() > static_cast<size_t>(config_.min_active)) {
      if (config_.min_active == 0) min_active_cutoff = best_weight;
      else {
        std::nth_element(tmp_array_.begin(),
                         tmp_array_.begin() + config_.min_active,
                         tmp_array_.size() > static_cast<size_t>(config_.max_active) ?
                         tmp_array_.begin() + config_.max_active :
                         tmp_array_.end());
        min_active_cutoff = tmp_array_[config_.min_active];
      }
    }

    if (min_active_cutoff > beam_cutoff) { // min_active is looser than beam.
      if (adaptive_beam)
        *adaptive_beam = min_active_cutoff - best_weight + config_.beam_delta;
      return min_active_cutoff;
    } else {
      *adaptive_beam = config_.beam;
      return beam_cutoff;
    }
  }
}


WfstStateId DecCore::PropagateLm(WfstStateId lm_state, WfstArc *arc) {
  if (arc->olabel == kWfstEpsilon) {
    return lm_state; // no change in LM state if no word crossed.
  } else { // Propagate in the LM-diff FST.
    Arc lm_arc;
    bool ans = lm_fst_->GetArc(lm_state, arc->olabel, &lm_arc);
    if (!ans) { // this case is unexpected for statistical LMs.
      KALDI_LOG << "No arc available in LM (unlikely to be correct "
                   "if a statistical language model);";
      exit(0);
    } else {
      arc->weight = arc->weight + lm_arc.weight.Value();
      //TODO: ? KALDI_ASSERT(arc->olabel == lm_arc.olabel);
      arc->olabel = lm_arc.olabel; // probably will be the same.
      return lm_arc.nextstate; // return the new LM state.
    }
  }
}


BaseFloat DecCore::ProcessEmitting(DecodableInterface *decodable) {
  KALDI_ASSERT(token_net_.size() > 0);
  int32 frame = token_net_.size() - 1; // zero-based, to get likelihoods from the decodable object.
  token_net_.resize(token_net_.size() + 1);

  Elem *prev_toks = token_set_.Clear(); // transfer elems from hash to list
  Elem *best_elem = NULL;
  BaseFloat adaptive_beam;
  size_t tok_cnt;
  BaseFloat cur_cutoff = GetCutoff(prev_toks, &tok_cnt, &adaptive_beam, &best_elem);
  PossiblyResizeHash(tok_cnt);  // This makes sure the hash is always big enough.

  BaseFloat next_cutoff = std::numeric_limits<BaseFloat>::infinity();
  // pruning "online" before having seen all tokens

  BaseFloat cost_offset = 0.0; // keep probabilities in a good dynamic range.

  // First process the best token to get a hopefully
  // reasonably tight bound on the next cutoff.  The only
  // products of the next block are "next_cutoff" and "cost_offset".
  if (best_elem) {
    ViterbiState state = best_elem->key;
    WfstStateId la_state = ExtractLaState(state),
                lm_state = ExtractLmState(state);

    Token *tok = best_elem->val;
    cost_offset = - tok->total_cost;

    const WfstState *s = la_fst_->State(la_state);
    const WfstArc *a   = la_fst_->Arc(s->arc_base);

    for (int32 j = 0; j < s->num_arcs; j++,a++) {
      WfstArc arc = *a;
      if (arc.ilabel != kWfstEpsilon) {  // propagate..
        if (lm_fst_ != NULL) {
          PropagateLm(lm_state, &arc);
        }
        BaseFloat new_weight = tok->total_cost + arc.weight + (-decodable->LogLikelihood(frame, arc.ilabel)) + cost_offset;
        if (new_weight + adaptive_beam < next_cutoff) {
          next_cutoff = new_weight + adaptive_beam;
        }
      }
    }
  }

  // Store the offset on the acoustic likelihoods that we're applying.
  // Could just do cost_offsets_.push_back(cost_offset), but we
  // do it this way as it's more robust to future code changes.
  cost_offsets_.resize(frame + 1, 0.0);
  cost_offsets_[frame] = cost_offset;

  for (Elem *e = prev_toks, *e_tail; e != NULL; e = e_tail) {
    ViterbiState state = e->key;
    Token *tok = e->val;

    WfstStateId la_state = ExtractLaState(state),
                lm_state = ExtractLmState(state);

    if (tok->total_cost <= cur_cutoff) {
      const WfstState *s = la_fst_->State(la_state);
      const WfstArc *a   = la_fst_->Arc(s->arc_base);

      for (int32 j = 0; j < s->num_arcs; j++, a++) {
        WfstArc arc = *a;
        if (arc.ilabel != kWfstEpsilon) { // emitting
          WfstStateId next_la_state = arc.dst;
          WfstStateId next_lm_state = (lm_fst_ == NULL) ? 0 : PropagateLm(lm_state, &arc);
          
          BaseFloat ac_cost = cost_offset + (-decodable->LogLikelihood(frame, arc.ilabel)),
                    graph_cost = arc.weight,
                    cur_cost = tok->total_cost,
                    total_cost = cur_cost + ac_cost + graph_cost;

          if (total_cost > next_cutoff) {
            continue;
          } else if (total_cost + adaptive_beam < next_cutoff) { // prune by best current token
            next_cutoff = total_cost + adaptive_beam;
          }

          ViterbiState next_state = ComposeViterbiState(next_la_state, next_lm_state);
          Token *dst_tok = FindOrAddToken(next_state, frame + 1, total_cost, tok, NULL);
          tok->links = NewLink(dst_tok, arc.ilabel, arc.olabel, graph_cost, ac_cost, tok->links);
        }
      } // for all arcs
    }
    e_tail = e->tail;
    token_set_.Delete(e);
  } // for all active token
  return next_cutoff;
}


void DecCore::ProcessNonemitting(BaseFloat cutoff) {
  KALDI_ASSERT(!token_net_.empty());
  int32 cur_time = static_cast<int32>(token_net_.size()) - 1;

  KALDI_ASSERT(queue_.empty());
  for (const Elem *e = token_set_.GetList(); e != NULL;  e = e->tail) {
    queue_.push_back(e->key);
  }
  if (queue_.empty()) {
    if (!warned_) {
      KALDI_WARN << "Error, no surviving tokens at time " << cur_time;
      warned_ = true;
    }
  }

  while (!queue_.empty()) {
    ViterbiState state = queue_.back();
    queue_.pop_back();
    Token *tok = token_set_.Find(state)->val;

    if (tok->total_cost > cutoff) {
      continue;
    }

    DeleteLinksFromToken(tok);
    tok->links = NULL;

    WfstStateId la_state = ExtractLaState(state),
                lm_state = ExtractLmState(state);

    const WfstState *s = la_fst_->State(la_state);
    const WfstArc   *a = la_fst_->Arc(s->arc_base);

    for (int32 j = 0; j < s->num_arcs; j++, a++) {
      WfstArc arc = *a;
      if (arc.ilabel == kWfstEpsilon) {  // non-emitting
        WfstStateId next_la_state = arc.dst;
        WfstStateId next_lm_state = (lm_fst_ == NULL) ? 0 : PropagateLm(lm_state, &arc);
        
        BaseFloat total_cost = tok->total_cost + arc.weight;

        if (total_cost < cutoff) {
          ViterbiState next_state = ComposeViterbiState(next_la_state, next_lm_state);
          
          bool changed;
          Token *dst_tok = FindOrAddToken(next_state, cur_time, total_cost, tok, &changed);
          tok->links = NewLink(dst_tok, 0, arc.olabel, arc.weight, 0, tok->links);
          // "changed" tells us whether the new token has a different
          // cost from before, or is new [if so, add into queue].
          if (changed) queue_.push_back(next_state);
        }
      }
    } // for all arcs
  } // while queue not empty
}


void DecCore::DeleteElems(Elem *list) {
  for (Elem *e = list, *e_tail; e != NULL; e = e_tail) {
    e_tail = e->tail;
    token_set_.Delete(e);
  }
}


void DecCore::ClearTokenNet() {
  for (size_t t = 0; t < token_net_.size(); t++) {
    for (Token *tok = token_net_[t].toks; tok != NULL; ) {
      DeleteLinksFromToken(tok);
      Token *next_tok = tok->next;
      DeleteToken(tok);
      num_toks_--;
      tok = next_tok;
    }
  }
  token_net_.clear();
  KALDI_ASSERT(num_toks_ == 0);
}


// static
void DecCore::TopSortTokens(Token *tok_list, std::vector<Token*> *topsorted_list) {
  unordered_map<Token*, int32> token2pos;
  typedef unordered_map<Token*, int32>::iterator IterType;
  int32 num_toks = 0;
  for (Token *tok = tok_list; tok != NULL; tok = tok->next)
    num_toks++;
  int32 cur_pos = 0;
  // We assign the tokens numbers num_toks - 1, ... , 2, 1, 0.
  // This is likely to be in closer to topological order than
  // if we had given them ascending order, because of the way
  // new tokens are put at the front of the list.
  for (Token *tok = tok_list; tok != NULL; tok = tok->next)
    token2pos[tok] = num_toks - ++cur_pos;

  unordered_set<Token*> reprocess;

  for (IterType iter = token2pos.begin(); iter != token2pos.end(); ++iter) {
    Token *tok = iter->first;
    int32 pos = iter->second;
    for (ForwardLink *link = tok->links; link != NULL; link = link->next) {
      if (link->ilabel == 0) {
        // We only need to consider epsilon links, since non-epsilon links
        // transition between frames and this function only needs to sort a list
        // of tokens from a single frame.
        IterType following_iter = token2pos.find(link->dst_tok);
        if (following_iter != token2pos.end()) { // another token on this frame,
                                                 // so must consider it.
          int32 next_pos = following_iter->second;
          if (next_pos < pos) { // reassign the position of the next Token.
            following_iter->second = cur_pos++;
            reprocess.insert(link->dst_tok);
          }
        }
      }
    }
    // In case we had previously assigned this token to be reprocessed, we can
    // erase it from that set because it's "happy now" (we just processed it).
    reprocess.erase(tok);
  }

  size_t max_loop = 1000000, loop_count; // max_loop is to detect epsilon cycles.
  for (loop_count = 0;
       !reprocess.empty() && loop_count < max_loop; ++loop_count) {
    std::vector<Token*> reprocess_vec;
    for (unordered_set<Token*>::iterator iter = reprocess.begin();
         iter != reprocess.end(); ++iter)
      reprocess_vec.push_back(*iter);
    reprocess.clear();
    for (std::vector<Token*>::iterator iter = reprocess_vec.begin();
         iter != reprocess_vec.end(); ++iter) {
      Token *tok = *iter;
      int32 pos = token2pos[tok];
      // Repeat the processing we did above (for comments, see above).
      for (ForwardLink *link = tok->links; link != NULL; link = link->next) {
        if (link->ilabel == 0) {
          IterType following_iter = token2pos.find(link->dst_tok);
          if (following_iter != token2pos.end()) {
            int32 next_pos = following_iter->second;
            if (next_pos < pos) {
              following_iter->second = cur_pos++;
              reprocess.insert(link->dst_tok);
            }
          }
        }
      }
    }
  }
  KALDI_ASSERT(loop_count < max_loop && "Epsilon loops exist in your decoding "
               "graph (this is not allowed!)");

  topsorted_list->clear();
  topsorted_list->resize(cur_pos, NULL);  // create a list with NULLs in between.
  for (IterType iter = token2pos.begin(); iter != token2pos.end(); ++iter)
    (*topsorted_list)[iter->second] = iter->first;
}

} // namespace iot
} // namespace kaldi