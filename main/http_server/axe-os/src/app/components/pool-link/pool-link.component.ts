import { Component, ElementRef, OnDestroy, OnInit, ViewChild } from '@angular/core';
import { Subscription } from 'rxjs';
import { WebsocketService } from 'src/app/services/web-socket.service';
import { SystemApiService } from 'src/app/services/system.service';

interface Exchange {
  time: string;
  up: boolean;
  color: string;
  text: string;
}

@Component({
  selector: 'app-pool-link',
  templateUrl: './pool-link.component.html',
  styleUrl: './pool-link.component.scss'
})
export class PoolLinkComponent implements OnInit, OnDestroy {

  @ViewChild('cable') cableRef?: ElementRef<HTMLElement>;

  public poolUrl = '—';
  public poolPort: string | number = '';
  public worker = '—';
  public poolDiff = '—';
  public fallback = false;
  public connected = false;
  public hashrate = '—';
  public accepted = 0;
  public rejected = 0;
  public bestSession = '—';
  public jobs = 0;
  public latency = '— ms';
  public feed: Exchange[] = [];

  private readonly C = { up: '#35d0c0', ok: '#41d68a', bad: '#f2735f', job: '#9b8bff' };
  private wsSub?: Subscription;
  private pollTimer?: any;
  private buffer = '';
  private lastPrevhash = '';

  constructor(private ws: WebsocketService, private sys: SystemApiService) {}

  ngOnInit(): void {
    this.wsSub = this.ws.ws$.subscribe({
      next: (val: string) => {
        this.buffer += val;
        let idx: number;
        while ((idx = this.buffer.indexOf('\n')) >= 0) {
          const line = this.buffer.slice(0, idx);
          this.buffer = this.buffer.slice(idx + 1);
          this.parseLine(line);
        }
      },
      error: () => { /* la liaison passe simplement en "déconnecté" via le poll */ }
    });
    this.poll();
    this.pollTimer = setInterval(() => this.poll(), 5000);
  }

  ngOnDestroy(): void {
    this.wsSub?.unsubscribe();
    if (this.pollTimer) {
      clearInterval(this.pollTimer);
    }
  }

  private poll(): void {
    this.sys.getInfo('').subscribe({
      next: (i: any) => this.applyInfo(i),
      error: () => { this.connected = false; }
    });
  }

  private applyInfo(i: any): void {
    if (!i) {
      return;
    }
    this.connected = true;
    this.fallback = !!i.isUsingFallbackStratum;
    this.poolUrl = (this.fallback ? i.fallbackStratumURL : i.stratumURL) || '—';
    this.poolPort = (this.fallback ? i.fallbackStratumPort : i.stratumPort) ?? '';
    this.worker = this.shortWorker((this.fallback ? i.fallbackStratumUser : i.stratumUser) || '');
    const diff = i.poolDifficulty ?? i.stratumDiff ?? i.stratumDifficulty;
    this.poolDiff = diff != null ? (diff < 100000 ? Math.round(diff).toString() : this.human(diff)) : '—';
    this.hashrate = i.hashRate != null ? this.human(i.hashRate * 1e9, 'H/s') : '—';
    this.accepted = i.sharesAccepted ?? this.accepted;
    this.rejected = i.sharesRejected ?? this.rejected;
    this.bestSession = i.bestSessionDiff != null ? this.human(i.bestSessionDiff) : '—';
  }

  // Le flux WS porte les vrais messages Stratum : on en fait des paquets + un journal.
  private parseLine(line: string): void {
    const l = line.replace(/\x1b\[[0-9;]*m/g, '');
    let m: RegExpMatchArray | null;
    if (l.indexOf('mining.submit') >= 0 && l.indexOf('tx:') >= 0) {
      m = l.match(/"id":\s*(\d+)/);
      this.event(true, this.C.up, 'share envoyée' + (m ? ' #' + m[1] : ''));
    } else if (l.indexOf('message result accepted') >= 0) {
      this.event(false, this.C.ok, 'acceptée ✓ ' + this.latency);
    } else if (l.indexOf('message result rejected') >= 0) {
      this.event(false, this.C.bad, 'refusée ✗');
    } else if ((m = l.match(/Stratum response time:\s*([\d.]+)\s*ms/))) {
      this.latency = m[1] + ' ms';
    } else if (l.indexOf('mining.notify') >= 0) {
      const pm = l.match(/"params":\s*\[\s*"[0-9A-Fa-f]*"\s*,\s*"([0-9A-Fa-f]+)"/);
      const prevhash = pm ? pm[1] : '';
      const newBlock = !!prevhash && prevhash !== this.lastPrevhash && this.lastPrevhash !== '';
      if (prevhash) {
        this.lastPrevhash = prevhash;
      }
      this.jobs++;
      this.event(false, this.C.job, newBlock ? 'nouveau bloc ⛓' : 'nouveau job');
    }
  }

  private event(up: boolean, color: string, text: string): void {
    this.spawnPacket(up, color);
    this.feed.unshift({ time: new Date().toLocaleTimeString('fr-FR'), up, color, text });
    if (this.feed.length > 9) {
      this.feed.pop();
    }
  }

  // Un paquet lumineux traverse le câble (miner->pool si up, sinon pool->miner).
  private spawnPacket(up: boolean, color: string): void {
    const cable = this.cableRef?.nativeElement;
    if (!cable) {
      return;
    }
    const d = document.createElement('div');
    d.className = 'pool-link__pkt';
    d.style.background = color;
    d.style.boxShadow = '0 0 9px ' + color;
    cable.appendChild(d);
    try {
      const anim = d.animate(
        [
          { left: up ? '2%' : '98%', opacity: 0.15 },
          { offset: 0.12, opacity: 1 },
          { offset: 0.88, opacity: 1 },
          { left: up ? '98%' : '2%', opacity: 0.15 }
        ],
        { duration: 1100, easing: 'linear' });
      anim.onfinish = () => d.remove();
    } catch {
      d.remove();
    }
  }

  private shortWorker(user: string): string {
    if (!user) {
      return '—';
    }
    if (user.indexOf('.') >= 0) {
      return user.substring(user.lastIndexOf('.') + 1) || '—';
    }
    return user.length > 14 ? user.substring(0, 6) + '…' + user.slice(-4) : user;
  }

  private human(v: number, unit: string = ''): string {
    const a = Math.abs(v);
    let s: string;
    if (a >= 1e12) { s = (v / 1e12).toFixed(2) + ' T'; }
    else if (a >= 1e9) { s = (v / 1e9).toFixed(2) + ' G'; }
    else if (a >= 1e6) { s = (v / 1e6).toFixed(1) + ' M'; }
    else if (a >= 1e3) { s = (v / 1e3).toFixed(1) + ' k'; }
    else { s = Math.round(v).toString(); }
    return unit ? s + unit : s;
  }
}
