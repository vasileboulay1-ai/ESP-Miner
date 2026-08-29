import { Component, OnDestroy, OnInit } from '@angular/core';
import { SystemApiService } from 'src/app/services/system.service';

@Component({
  selector: 'app-network-status',
  templateUrl: './network-status.component.html',
  styleUrl: './network-status.component.scss'
})
export class NetworkStatusComponent implements OnInit, OnDestroy {

  public active = 'Wi-Fi';
  public ethEnabled = false;
  public ethLinkUp = false;
  public ethHasIp = false;
  public ethIp = '';
  public ethGateway = '';
  public ethMac = '';
  public ethChipset = '';

  public ssid = '';
  public wifiIp = '';
  public rssi = 0;
  public wifiStatus = '';

  private timer?: any;

  constructor(private sys: SystemApiService) {}

  ngOnInit(): void {
    this.poll();
    this.timer = setInterval(() => this.poll(), 5000);
  }

  ngOnDestroy(): void {
    if (this.timer) { clearInterval(this.timer); }
  }

  public get onEthernet(): boolean {
    return this.active === 'Ethernet';
  }

  public get rssiLabel(): string {
    if (!this.rssi) { return '—'; }
    if (this.rssi >= -50) { return 'excellent'; }
    if (this.rssi >= -60) { return 'très bon'; }
    if (this.rssi >= -67) { return 'bon'; }
    if (this.rssi >= -75) { return 'moyen'; }
    return 'faible';
  }

  private poll(): void {
    this.sys.getInfo('').subscribe({
      next: (i: any) => {
        if (!i) { return; }
        this.active = i.activeNetwork || 'Wi-Fi';
        this.ethEnabled = !!i.ethEnabled;
        this.ethLinkUp = !!i.ethLinkUp;
        this.ethHasIp = !!i.ethHasIp;
        this.ethIp = i.ethIp || '';
        this.ethGateway = i.ethGateway || '';
        this.ethMac = i.ethMac || '';
        this.ethChipset = i.ethChipset || '';
        this.ssid = i.ssid || '';
        this.wifiIp = i.hostip || '';
        this.rssi = i.wifiRSSI ?? 0;
        this.wifiStatus = i.wifiStatus || '';
      },
      error: () => { /* carte inactive */ }
    });
  }
}
