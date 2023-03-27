// Copyright 2021 Optiver Asia Pacific Pty. Ltd.
//
// This file is part of Ready Trader Go.
//
//     Ready Trader Go is free software: you can redistribute it and/or
//     modify it under the terms of the GNU Affero General Public License
//     as published by the Free Software Foundation, either version 3 of
//     the License, or (at your option) any later version.
//
//     Ready Trader Go is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU Affero General Public License for more details.
//
//     You should have received a copy of the GNU Affero General Public
//     License along with Ready Trader Go.  If not, see
//     <https://www.gnu.org/licenses/>.
#include <array>
#include <algorithm>
#include<chrono> 
#include <thread> 
#include <iostream>
#include <boost/asio/io_context.hpp>

#include <ready_trader_go/logging.h>

#include "autotrader.h"

using namespace std;
using namespace ReadyTraderGo;


RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")


constexpr int POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;
constexpr int MIN_BID_NEAREST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int MAX_ASK_NEAREST_TICK = (MAXIMUM_ASK - TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
const int MARGIN = 240;
typedef long long ll;

int r100(int x){
    return ((x+50)/100)*100;
}

void AutoTrader::cancellation_loop(){
    auto target = std::chrono::high_resolution_clock::now();
    while (1) {
        if(limiter.check_remaining() < 15) {
            std::this_thread::sleep_for(chrono::microseconds(21000 / SPEED));
            continue;
        }
        if (last_future_info < std::chrono::high_resolution_clock::now() - chrono::microseconds(20000 / SPEED)){
            if(!start) continue;
            //RLOG(LG_AT, LogLevel::LL_INFO) << "Plan to get info" ;
            cc^=1;
            const std::lock_guard<std::mutex> lock(mtx);
            int id = mNextMessageId++;
            if(current_hedge == 100 && cc == 0) continue;
            if(current_hedge == -100 && cc == 1) continue;
            if(cc){
                rate_limited_hedge(id, Side::SELL, r100(theo_fut + 100), 1);
                hedgeS.insert(id);
            } else {
                rate_limited_hedge(id, Side::BUY, r100(theo_fut - 100), 1);
                hedgeB.insert(id);
            }
        }
        std::this_thread::sleep_for(chrono::microseconds(21000 / SPEED));
    }
}

bool AutoTrader::rate_limited_cancel(unsigned long clientOrderId){
    if (limiter.check_and_add()){
        SendCancelOrder(clientOrderId);
        return true;
    }
    return false;
}

bool AutoTrader::rate_limited_hedge(unsigned long clientOrderId,  Side side, unsigned long price, unsigned long volume){
    if(future_l) return false;

    if (limiter.check_and_add()){
        future_l = true;
        SendHedgeOrder(clientOrderId, side, price, volume);
        return true;
    }
    return false;
}

bool AutoTrader::rate_limited_insert(unsigned long clientOrderId, Side side, unsigned long price, unsigned long volume, Lifespan lifespan){
    if (limiter.check_and_add()){
        SendInsertOrder(clientOrderId, side, price, volume, lifespan);
        return true;
    }
    return false;
}

AutoTrader::AutoTrader(boost::asio::io_context& context) : BaseAutoTrader(context)
{
   cloop = thread(&AutoTrader::cancellation_loop, this);
}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
    RLOG(LG_AT, LogLevel::LL_INFO) << "execution connection lost";
}

void AutoTrader::ErrorMessageHandler(unsigned long clientOrderId,
                                     const std::string& errorMessage)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "error with order " << clientOrderId << ": " << errorMessage;
}



void AutoTrader::test_place_order(){
    if(newAskPrice <= newBidPrice) return;
    if(limiter.check_remaining() < 2) return;

    int ask_target = (mPosition + POSITION_LIMIT + 1)/2;

    if (!(mPosition <= 0 && asks.cnt() != 0)){
        if (mPosition <= 0 ) ask_target = 200;
        if ( newAskPrice!=0 && asks.cnt() < 2 && (!asks.contains_price(newAskPrice)) ) {
            int ask = min(ask_target, mPosition + POSITION_LIMIT - asks.totsz());
            
            if (ask && rate_limited_insert(mNextMessageId, Side::SELL, newAskPrice, ask, Lifespan::GOOD_FOR_DAY)) {
                asks.insert(mNextMessageId, newAskPrice, ask);
                mNextMessageId+=1;
            }

        }
    }

    int bid_target = (POSITION_LIMIT - mPosition + 1)/2;

    if(!(mPosition>=0 && bids.cnt() != 0)){
        if(mPosition>=0) ask_target = 200;
        if(newBidPrice !=0 && bids.cnt() < 2 && (!bids.contains_price(newBidPrice))) {
            int bid = min(bid_target, POSITION_LIMIT-mPosition - bids.totsz());

            if(bid && rate_limited_insert(mNextMessageId, Side::BUY, newBidPrice, bid, Lifespan::GOOD_FOR_DAY)) {
                bids.insert(mNextMessageId, newBidPrice, bid);
                mNextMessageId+=1;
            }
        }
    }
    
}


double weight[5] = {0.35,0.25,0.15,0.10,0.05};
double calc_vwap(const std::array<unsigned long, TOP_LEVEL_COUNT> Prices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT> Volumes)
{
    double A = 0, B = 0;
    for(int i=0;i<5;i++){
        A += weight[i]*Prices[i]*Volumes[i];
        B += weight[i]*Volumes[i];
    }
    if(B < 1) return 0;
    return A/B;
}

int get_h(int c){
    return min(96, max(-96, -c));
}

void AutoTrader::try_hedge(){

    if(target_hedge<current_hedge){
        int id = mNextMessageId++;
        rate_limited_hedge(id, Side::SELL, ((theo_fut+50)/100)*100, current_hedge-target_hedge);
        hedgeS.insert(id);
    }

    if(target_hedge>current_hedge){
        int id = mNextMessageId++;
        rate_limited_hedge(id, Side::BUY, ((theo_fut+50)/100)*100, target_hedge - current_hedge);
        hedgeB.insert(id);
    }
}

void AutoTrader::cancel_and_place(){
    newAskPrice = r100(theo + MARGIN);
    newBidPrice = r100(theo - MARGIN);
    
    if(mPosition < 0) newBidPrice+=100;
    if(mPosition > 0) newAskPrice-=100;

    if (newAskPrice != 0) {
        vector<int> atcs = asks.to_cancel_ne(newAskPrice);
        for(auto atc: atcs){
            if ( atc && rate_limited_cancel(atc) ) asks.cancel(atc);
        }
    }

    if (newBidPrice != 0) {
        vector<int> atcs = bids.to_cancel_ne(newBidPrice);
        for(auto atc: atcs){
            if ( atc && rate_limited_cancel(atc) ) bids.cancel(atc);
        }
    }

    if(start) test_place_order();
}

void AutoTrader::new_fut_price(int maxask, int askvol, int minbid, int bidvol){
    if(minbid == 1e9 && maxask == 0) return;
    if(maxask == 0) theo_fut = min(theo_fut, minbid);
    if(minbid == 1e9) theo_fut = max(theo_fut, maxask);
    if(maxask != 0 && minbid != 1e9){
        if (askvol > bidvol) theo_fut = max(theo_fut, maxask);
        else theo_fut = min(theo_fut, minbid);
    }

    last_future_info = std::chrono::high_resolution_clock::now();
    theo = (theo_fut*2 + theo_etf*2)/4;

    cancel_and_place();
}


void AutoTrader::HedgeFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    const std::lock_guard<std::mutex> lock(mtx);
    
    // RLOG(LG_AT, LogLevel::LL_INFO) << "hedge order " << clientOrderId << " filled for " << volume
    //                                << " lots at $" << price << " average price in cents";

    int maxask = 0, askvol = 0, minbid = 1e9, bidvol = 0;
    if (hedgeS.count(clientOrderId) == 1) {
        current_hedge -= (long)volume;
        maxask = price + 102;
        askvol = 1;
    } else if (hedgeB.count(clientOrderId) == 1) {
        current_hedge += (long)volume;
        minbid = price - 102;
        bidvol = 1;
    }

    future_l = 0;

    if(volume == 1){
        new_fut_price(maxask, askvol, minbid, bidvol);
    }

}


void AutoTrader::OrderBookMessageHandler(Instrument instrument,
                                         unsigned long sequenceNumber,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    const std::lock_guard<std::mutex> lock(mtx);
    
    // RLOG(LG_AT, LogLevel::LL_INFO) << "order book received for " << instrument << " instrument"
    //                                << ": ask prices: " << askPrices[0]
    //                                << "; ask volumes: " << askVolumes[0]
    //                                << "; bid prices: " << bidPrices[0]
    //                                << "; bid volumes: " << bidVolumes[0];

    
    

    if (instrument == Instrument::FUTURE) {
        //int theo = (1ll*askPrices[0]*askVolumes[0] + 1ll*bidPrices[0]*bidVolumes[0])/(askVolumes[0] + bidVolumes[0]);
        theo_fut = (calc_vwap(askPrices,askVolumes) + calc_vwap(bidPrices,bidVolumes))/2;
        last_future_info = std::chrono::high_resolution_clock::now();
        //int theo = (1ll*askPrices[0] + 1ll*bidPrices[0])/2;
    }
    
    if (instrument == Instrument::ETF) {
        std::array<unsigned long, TOP_LEVEL_COUNT> aV = askVolumes;
        std::array<unsigned long, TOP_LEVEL_COUNT> bV = bidVolumes;
        
        for(int i=0;i<5;i++)aV[i] = min(250,(int)aV[i]);
        for(int i=0;i<5;i++)bV[i] = min(250,(int)bV[i]);
        etfA = calc_vwap(askPrices,aV);
        etfB = calc_vwap(bidPrices,bV);

        theo_etf = (etfA+etfB)/2;
        theo = (theo_fut*2 + theo_etf*2)/4;
        
        if(sequenceNumber > 5){
            start = 1;
        }
        
        cancel_and_place();
        try_hedge();
    }
}


void AutoTrader::OrderFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    const std::lock_guard<std::mutex> lock(mtx);
    
    // RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " filled for " << volume
    //                                << " lots at $" << price << " cents";
    
    if (asks.contains(clientOrderId)) {
        mPosition -= (long)volume;
    }
    
    if (bids.contains(clientOrderId)) {
        mPosition += (long)volume;
    }

    target_hedge = get_h(mPosition);
}

void AutoTrader::OrderStatusMessageHandler(unsigned long clientOrderId,
                                           unsigned long fillVolume,
                                           unsigned long remainingVolume,
                                           signed long fees)
{
    const std::lock_guard<std::mutex> lock(mtx);
    
    // RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " filled for " << fillVolume
    //                                << " lots at $" << fees << " fees";
    
    if(asks.contains(clientOrderId)){
        asks.update(clientOrderId, remainingVolume);
    }

    if(bids.contains(clientOrderId)){
        bids.update(clientOrderId, remainingVolume);
    }

    test_place_order();
}

// TODO: Use this data to infer the current price of future / etf, and to update order accordingly
void AutoTrader::TradeTicksMessageHandler(Instrument instrument,
                                          unsigned long sequenceNumber,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    const std::lock_guard<std::mutex> lock(mtx);
    
    // RLOG(LG_AT, LogLevel::LL_INFO) << "trade ticks received for " << instrument << " instrument"
    //                                << ": ask prices: " << askPrices[0]
    //                                << "; ask volumes: " << askVolumes[0]
    //                                << "; bid prices: " << bidPrices[0]
    //                                << "; bid volumes: " << bidVolumes[0];

    if (instrument == Instrument::FUTURE) {
        // for(int i=0;i<5;i++) cout<<askPrices[i]<<'\t'; cout<<endl;
        // for(int i=0;i<5;i++) cout<<askVolumes[i]<<'\t'; cout<<endl;
        // for(int i=0;i<5;i++) cout<<bidPrices[i]<<'\t'; cout<<endl;
        // for(int i=0;i<5;i++) cout<<bidVolumes[i]<<'\t'; cout<<endl;

        int maxask = 0, askvol = 0;
        int minbid = 1e9, bidvol = 0;

        // cout << theo_fut; 
        for(int i=0;i<5;i++){
            if(askVolumes[i] > 200) maxask = max(maxask, (int)askPrices[i]);
            askvol += askVolumes[i];
        }
        for(int i=0;i<5;i++){
            if(bidVolumes[i] > 200) minbid = min(minbid, (int)bidPrices[i]);
            bidvol += bidVolumes[i];
        }

        if(limiter.check_remaining() < 8) return;
        new_fut_price(maxask, askvol, minbid, bidvol);
    }
}
