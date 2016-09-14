// This work is licensed under the Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License.
// To view a copy of this license, visit http://creativecommons.org/licenses/by-nc-nd/4.0/ or send a letter to
// Creative Commons, PO Box 1866, Mountain View, CA 94042, USA.

#include "pkevent.h"
#include "RcppInclude.h"
#include "odeproblem.h"
#include "mrgsolve.h"

#define N_SS 1000
#define CRIT_DIFF_SS 1E-10



pkevent::pkevent(short int cmt_,
                 unsigned int evid_,
                 double amt_,
                 double time_,
                 double rate_,
                 int pos_,
                 double id_): datarecord(evid_, time_, cmt_, pos_, id_) {

  Amt = amt_;
  Rate = rate_;
  Ss = 0;
  Addl = 0;
  Ii = 0.0;
  Fn = 1.0;
  Armed = true;
}

pkevent::pkevent(short int cmt_,
                 unsigned int evid_,
                 double amt_,
                 double time_,
                 double rate_): datarecord(evid_, time_, cmt_) {
  
  Amt = amt_;
  Rate = rate_;
  Ss = 0;
  Addl = 0;
  Ii = 0.0;
  Fn = 1.0;
  Armed = true;
}



double pkevent::dur(double b) {
  return(b*Amt/Rate);
}

void pkevent::implement(odeproblem *prob) {
  
  if(!Armed) return;
  
  unsigned int evid = Evid;
  
  if(this->infusion()) evid = 5;
  
  int eq_n = std::abs(Cmt)-1;
  
  // Check for steady state records:
  if(Ss==1) {
    if(Fn==0) Rcpp::stop("Cannot use ss flag when F(n) is zero.");
    if(Rate == 0) this->steady_bolus(prob);
    if(Rate >  0) this->steady_infusion(prob);
  }
  
  switch (evid) {
  case 1: // Dosing event record
    if(!prob->is_on(eq_n)) Rcpp::stop("Attemped bolus dose into a compartment that is off");
    prob->fbio(eq_n, Fn);
    prob->y(eq_n,(prob->y(eq_n)  + Amt * Fn));
    break;
  case 5:  // Turn infusion on event record
    if(!prob->is_on(eq_n)) Rcpp::stop("Attemped infusion start for a compartment that is off");
    if(Fn == 0) break;
    prob->fbio(eq_n, Fn);
    prob->rate_add(eq_n,Rate);
    break;
  case 9: // Turn infusion off event record
    if(!prob->is_on(eq_n)) break;
    prob->rate_rm(eq_n, Rate);
    break;
  case 2: // Other type event record:
    if(Cmt > 0) { // trn the compartment on
      prob->on(eq_n);
    }
    if(Cmt < 0) {
      prob->off(eq_n);
      prob->y(eq_n,0.0);
    }
    break;
  case 3: // reset event record
    for(int i=0; i < prob->neq(); ++i) {
      prob->y(i,0.0);
      prob->on(i);
      prob->rate0(i,0.0);
      
    }
    {
      //double tm = Time;
      prob->newind(1);
      prob->init_call(Time);
    }
    break;
  case 8: // replace
    prob->y(eq_n, Amt);
    break;
  case 4:
    for(int i=0; i < prob->neq(); ++i) {
      prob->y(i,0.0);
      prob->on(i);
      prob->rate0(i,0.0);
    } {
      //double tm = this->time();
      //prob->newind(2);
      prob->init_call(Time);
      if(Rate > 0) {
        this->evid(5);
      } else {
        this->evid(1);
      }
      this-> implement(prob);
      return;
    }
    
  case 11:
    prob->y(eq_n,(prob->y(eq_n)  + prob->xdose()));
    break;
  }
  prob->lsoda_init();
}

void pkevent::steady_bolus(odeproblem *prob) {
  
  prob->rate_reset();
  
  //double ii = this->ii();
  double tfrom = 0.0;
  double tto = 0.0;
  int i;
  int j;
  std::vector<double> res(prob->neq(), 1E-9);
  std::vector<double> last(prob->neq(),1E-9);
  
  double this_sum = 0.0;
  double last_sum = 1E-6;
  double diff = 1E6;
  
  prob->lsoda_init();
  
  ev_ptr evon(new pkevent(Cmt, 1, Amt, Time, Rate));
  evon->fn(Fn);
  
  for(i=1; i < N_SS; ++i) {
    tfrom = double(i-1)*Ii;
    tto = double(i)*Ii;
    evon->implement(prob);
    prob->lsoda_init();
    prob->advance(tfrom,tto);
    
    for(j=0; j < prob->neq(); ++j) {
      res[j]  = pow(prob->y(j) - last[j],2);
      last[j] = prob->y(j);
    }
    
    this_sum = std::accumulate(res.begin(), res.end(), 0.0);
    
    if(i>10) {
      diff = std::abs(this_sum - last_sum);
      
      if((diff < CRIT_DIFF_SS)){
        break;
      }
    }
    
    tfrom = tto;
    last_sum = this_sum;
  }
  prob->itask(1);
}


void pkevent::steady_infusion(odeproblem *prob) {

  double duration = this->dur(Fn);
  
  double tfrom = 0.0;
  
  int i;
  int j;
  std::vector<double> res(prob->neq(), 0.0);
  std::vector<double> last(prob->neq(),1E-10);
  evlist offs;
  double this_sum = 0.0;
  double last_sum = 1E-6;
  
  double diff = 1E6;
  double nexti, toff;
  prob->rate_reset();
  // We only need one of these; it gets updated and re-used immediately
  ev_ptr evon(new pkevent(Cmt, 1, Amt, Time, Rate));
  
  for(i=1; i < N_SS ; ++i) {
    evon->time(tfrom);
    evon->implement(prob);
    prob->lsoda_init();
    toff = tfrom + duration;
    
    // Create an event to turn the infusion off and push onto offs vector
    // Keep on creating these
    ev_ptr evoff(new pkevent(Cmt, 9, Amt, toff, Rate));
    offs.push_back(evoff);
    
    // The next time an infusion will start
    nexti = double(i)*Ii;
    // As long as there are infusions to turn off and the
    // first one is before or at the next infusion start time
    while((!offs.empty()) && (offs[0]->time()  <= nexti)) {
      
      toff = offs[0]->time();
      prob->advance(tfrom,toff);
      offs[0]->implement(prob);
      prob->lsoda_init();
      tfrom = toff;
      offs.erase(offs.begin());
    }
    
    prob->lsoda_init();
    prob->advance(tfrom,nexti);
    
    tfrom = nexti;
    
    for(j=0; j < prob->neq(); ++j) {
      res[j]  = pow((prob->y(j)  - last[j]),2);
      last[j] = prob->y(j);
    }
    
    this_sum = std::accumulate(res.begin(), res.end(), 0.0);
    
    if(i>10) {
      diff = std::abs(this_sum - last_sum);
      if(diff < CRIT_DIFF_SS) {
        break;
      }
    }
    last_sum = this_sum;
  }
  prob->lsoda_init();
}



bool CompByTime(ev_ptr a, ev_ptr b) {return a->time() < b->time();}
bool CompByPos(ev_ptr a, ev_ptr b)  {return a->pos() < b->pos();  }

void pkevent::schedule(std::vector<rec_ptr>& thisi, double maxtime, bool put_ev_first) {
  
  int nextpos = put_ev_first ? -600 : (thisi.size() + 10);

  if(Fn==0) return;
  
  // End if infusion
  if(Rate > 0) {
    
    ev_ptr evoff(new pkevent(Cmt,
                             9, // EVID 9 means infusion off
                             Amt,
                             Time + this->dur(Fn),
                             Rate, 
                             -300, Id));
    
    //evoff->id(Id);
    //evoff->pos(-300);
    evoff->output(false);
    thisi.push_back(evoff);
    
    if(Ss) {
      
      double duration = this->dur(Fn);
      
      int ninf_ss = floor(duration/this->ii());
      
      double first_off = duration - double(ninf_ss)*Ii + Time;
      
      if(first_off == Time) {
        first_off = duration - Ii +  Time;
        --ninf_ss;
      }
      
      for(int k=0; k < ninf_ss; ++k) {
        
        double offtime = first_off+double(k)*double(Ii);
        
        ev_ptr evoff(new pkevent(Cmt,
                                 9, // EVID 9 means infusion off
                                 Amt,
                                 offtime,
                                 Rate, 
                                 -300, Id));
        
        //evoff->id(Id);
        //evoff->pos(-300);
        evoff->output(false);
        thisi.push_back(evoff);
        
      } // Done creating off infusions for end of steady_infusion
    } // end if(ss)
  } // end rate>0
  
  
  if(Addl > 0) {
    
    unsigned int this_evid = Evid;
    
    if(this_evid == 4) {
      this_evid = Rate > 0 ? 5 : 1;
    }
    
    if(this->infusion()) {
      thisi.reserve(thisi.size() + 2*Addl);  
    } else {
      thisi.reserve(thisi.size() + Addl); 
    }
    
    double ontime = 0;
    
    for(unsigned int k=1; k <= Addl; ++k) {
      
      ontime = Time + Ii*double(k);
      
      if(ontime > maxtime) break;
      
      ev_ptr evon(new pkevent(Cmt,
                              this_evid,
                              Amt,
                              ontime,
                              Rate,
                              nextpos,Id));
      
      //evon->id(Id);
      //evon->pos(nextpos);
      evon->fn(Fn);
      evon->output(false);
      
      thisi.push_back(evon);
      
      if(this->infusion()) {
        ev_ptr evoff(new pkevent(Cmt,
                                 9, // EVID 9 means infusion off
                                 Amt,
                                 ontime + this->dur(Fn),
                                 Rate, 
                                 -300,Id));
        
        //evoff->id(Id);
        //evoff->pos(-300);
        evoff->output(false);
        
        thisi.push_back(evoff);
      }
    }
  } // end addl
}

void add_mtime(reclist& thisi, dvec& b, dvec& c, bool debug) {
  
  if((b.size()==0) & (c.size()==0)) return;
  
  double maxtime  = thisi.back()->time();
  double mintime = thisi.at(0)->time();
  std::sort(b.begin(), b.end());
  std::sort(c.begin(), c.end());
  
  b.insert(b.end(), c.begin(), c.end() );
  std::sort(b.begin(), b.end());
  
  b.erase(unique(b.begin(), b.end()), b.end());
  
  std::size_t i = 0;
  
  //  if(debug) Rcpp::Rcout <<std::endl<< "Adding phantom observation records from mtime " << std::endl;
  
  bool dropmin = true;
  bool dropmax = true;
  
  // add mtimes from argument
  for(i=0; i < b.size(); ++i) {
    
    if(b.at(i) <= mintime) {
      if(debug && dropmin) {
        Rcpp::Rcout << "dropping mtimes <=  min observation time" << std::endl;
        dropmin = false;
      }
      continue;
    }
    
    if(b.at(i) >= maxtime)  {
      if(debug && dropmax) {
        Rcpp::Rcout << "dropping mtimes >= to max observation time" << std::endl;
        dropmax = false;
      }
      break;
    }
    
    //if(debug)  Rcpp::Rcout << "mtime: " << b[i] << std::endl;
    rec_ptr obs(new datarecord(100,b[i],0,-100,0));
    obs->output(false);
    thisi.push_back(obs);
  }
  // Sort
  std::sort(thisi.begin(), thisi.end(), CompByTimePosRec);
}


