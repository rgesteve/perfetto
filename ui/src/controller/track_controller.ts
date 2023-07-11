// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {BigintMath} from '../base/bigint_math';
import {assertExists} from '../base/logging';
import {Engine} from '../common/engine';
import {Registry} from '../common/registry';
import {RESOLUTION_DEFAULT, TraceTime, TrackState} from '../common/state';
import {
  Duration,
  duration,
  time,
  Time,
  TimeSpan,
} from '../common/time';
import {LIMIT, TrackData} from '../common/track_data';
import {globals} from '../frontend/globals';
import {publishTrackData} from '../frontend/publish';

import {Controller, ControllerFactory} from './controller';

interface TrackConfig {}

type TrackConfigWithNamespace = TrackConfig&{namespace: string};

// TrackController is a base class overridden by track implementations (e.g.,
// sched slices, nestable slices, counters).
export abstract class TrackController<
    Config extends TrackConfig, Data extends TrackData = TrackData> extends
    Controller<'main'> {
  readonly trackId: string;
  readonly engine: Engine;
  private data?: TrackData;
  private requestingData = false;
  private queuedRequest = false;
  private isSetup = false;
  private lastReloadHandled = 0;

  // We choose 100000 as the table size to cache as this is roughly the point
  // where SQLite sorts start to become expensive.
  private static readonly MIN_TABLE_SIZE_TO_CACHE = 100000;

  constructor(args: TrackControllerArgs) {
    super('main');
    this.trackId = args.trackId;
    this.engine = args.engine;
  }

  // Can be overriden by the track implementation to allow one time setup work
  // to be performed before the first onBoundsChange invcation.
  async onSetup() {}

  // Can be overriden by the track implementation to allow some one-off work
  // when requested reload (e.g. recalculating height).
  async onReload() {}

  // Must be overridden by the track implementation. Is invoked when the track
  // frontend runs out of cached data. The derived track controller is expected
  // to publish new track data in response to this call.
  abstract onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data>;

  get trackState(): TrackState {
    return assertExists(globals.state.tracks[this.trackId]);
  }

  get config(): Config {
    return this.trackState.config as Config;
  }

  configHasNamespace(config: TrackConfig): config is TrackConfigWithNamespace {
    return 'namespace' in config;
  }

  namespaceTable(tableName: string): string {
    if (this.configHasNamespace(this.config)) {
      return this.config.namespace + '_' + tableName;
    } else {
      return tableName;
    }
  }

  publish(data: Data): void {
    this.data = data;
    publishTrackData({id: this.trackId, data});
  }

  // Returns a valid SQL table name with the given prefix that should be unique
  // for each track.
  tableName(prefix: string) {
    // Derive table name from, since that is unique for each track.
    // Track ID can be UUID but '-' is not valid for sql table name.
    const idSuffix = this.trackId.split('-').join('_');
    return `${prefix}_${idSuffix}`;
  }

  shouldSummarize(resolution: number): boolean {
    // |resolution| is in s/px (to nearest power of 10) assuming a display
    // of ~1000px 0.0008 is 0.8s.
    return resolution >= 0.0008;
  }

  protected async query(query: string) {
    const result = await this.engine.query(query);
    return result;
  }

  private shouldReload(): boolean {
    const {lastTrackReloadRequest} = globals.state;
    return !!lastTrackReloadRequest &&
        this.lastReloadHandled < lastTrackReloadRequest;
  }

  private markReloadHandled() {
    this.lastReloadHandled = globals.state.lastTrackReloadRequest || 0;
  }

  shouldRequestData(traceTime: TraceTime): boolean {
    const tspan = new TimeSpan(traceTime.start, traceTime.end);
    if (this.data === undefined) return true;
    if (this.shouldReload()) return true;

    // If at the limit only request more data if the view has moved.
    const atLimit = this.data.length === LIMIT;
    if (atLimit) {
      // We request more data than the window, so add window duration to find
      // the previous window.
      const prevWindowStart = this.data.start + tspan.duration;
      return tspan.start !== prevWindowStart;
    }

    // Otherwise request more data only when out of range of current data or
    // resolution has changed.
    const inRange =
        tspan.start >= this.data.start && tspan.end <= this.data.end;
    return !inRange ||
        this.data.resolution !==
        globals.state.frontendLocalState.visibleState.resolution;
  }

  // Decides, based on the length of the trace and the number of rows
  // provided whether a TrackController subclass should cache its quantized
  // data. Returns the bucket size (in ns) if caching should happen and
  // undefined otherwise.
  // Subclasses should call this in their setup function
  calcCachedBucketSize(numRows: number): duration|undefined {
    // Ensure that we're not caching when the table size isn't even that big.
    if (numRows < TrackController.MIN_TABLE_SIZE_TO_CACHE) {
      return undefined;
    }

    const traceDuration = globals.stateTraceTimeTP().duration;

    // For large traces, going through the raw table in the most zoomed-out
    // states can be very expensive as this can involve going through O(millions
    // of rows). The cost of this becomes high even for just iteration but is
    // especially slow as quantization involves a SQLite sort on the quantized
    // timestamp (for the group by).
    //
    // To get around this, we can cache a pre-quantized table which we can then
    // in zoomed-out situations and fall back to the real table when zoomed in
    // (which naturally constrains the amount of data by virtue of the window
    // covering a smaller timespan)
    //
    // This method computes that cached table by computing an approximation for
    // the bucket size we would use when totally zoomed out and then going a few
    // resolution levels down which ensures that our cached table works for more
    // than the literally most zoomed out state. Moving down a resolution level
    // is defined as moving down a power of 2; this matches the logic in
    // |globals.getCurResolution|.
    //
    // TODO(lalitm): in the future, we should consider having a whole set of
    // quantized tables each of which cover some portion of resolution lvel
    // range. As each table covers a large number of resolution levels, even 3-4
    // tables should really cover the all concievable trace sizes. This set
    // could be computed by looking at the number of events being processed one
    // level below the cached table and computing another layer of caching if
    // that count is too high (with respect to MIN_TABLE_SIZE_TO_CACHE).

    // 4k monitors have 3840 horizontal pixels so use that for a worst case
    // approximation of the window width.
    const approxWidthPx = 3840n;

    // Compute the outermost bucket size. This acts as a starting point for
    // computing the cached size.
    const outermostBucketSize =
        BigintMath.bitCeil(traceDuration / approxWidthPx);
    const outermostResolutionLevel = BigintMath.log2(outermostBucketSize);

    // This constant decides how many resolution levels down from our outermost
    // bucket computation we want to be able to use the cached table.
    // We've chosen 7 as it seems to be empircally seems to be a good fit for
    // trace data.
    const resolutionLevelsCovered = 7n;

    // If we've got less resolution levels in the trace than the number of
    // resolution levels we want to go down, bail out because this cached
    // table is really not going to be used enough.
    if (outermostResolutionLevel < resolutionLevelsCovered) {
      return Duration.MAX;
    }

    // Another way to look at moving down resolution levels is to consider how
    // many sub-intervals we are splitting the bucket into.
    const bucketSubIntervals = 1n << resolutionLevelsCovered;

    // Calculate the smallest bucket we want our table to be able to handle by
    // dividing the outermsot bucket by the number of subintervals we should
    // divide by.
    const cachedBucketSize = outermostBucketSize / bucketSubIntervals;

    return cachedBucketSize;
  }

  run() {
    const visibleState = globals.state.frontendLocalState.visibleState;
    if (visibleState === undefined) {
      return;
    }
    const visibleTimeSpan = globals.stateVisibleTime();
    const dur = visibleTimeSpan.duration;
    if (globals.state.visibleTracks.includes(this.trackId) &&
        this.shouldRequestData(visibleState)) {
      if (this.requestingData) {
        this.queuedRequest = true;
      } else {
        this.requestingData = true;
        let promise = Promise.resolve();
        if (!this.isSetup) {
          promise = this.onSetup();
        } else if (this.shouldReload()) {
          promise = this.onReload().then(() => this.markReloadHandled());
        }
        promise
            .then(() => {
              this.isSetup = true;
              let resolution = visibleState.resolution;

              // If resolution is not a power of 2, reset to the default value
              if (BigintMath.popcount(resolution) !== 1) {
                resolution = RESOLUTION_DEFAULT;
              }

              return this.onBoundsChange(
                  Time.sub(visibleTimeSpan.start, dur),
                  Time.add(visibleTimeSpan.end, dur),
                  resolution);
            })
            .then((data) => {
              this.publish(data);
            })
            .finally(() => {
              this.requestingData = false;
              if (this.queuedRequest) {
                this.queuedRequest = false;
                this.run();
              }
            });
      }
    }
  }
}

export interface TrackControllerArgs {
  trackId: string;
  engine: Engine;
}

export interface TrackControllerFactory extends
    ControllerFactory<TrackControllerArgs> {
  kind: string;
}

export const trackControllerRegistry =
    Registry.kindRegistry<TrackControllerFactory>();
