from __future__ import annotations
import HEngine.RuntimeResourceWarmup
import typing

__all__ = [
    "Failed",
    "GetGroupProgress",
    "GetTriggerProgress",
    "GroupProgress",
    "Idle",
    "Loading",
    "QueueTrigger",
    "Queued",
    "Ready",
    "ReleaseGroup",
    "Released",
    "RethrowFailure",
    "State",
    "TriggerProgress"
]


class GroupProgress():
    @property
    def Dispatched(self) -> int:
        """
        :type: int
        """
    @property
    def Failed(self) -> int:
        """
        :type: int
        """
    @property
    def Loading(self) -> int:
        """
        :type: int
        """
    @property
    def Ratio(self) -> float:
        """
        :type: float
        """
    @property
    def Ready(self) -> int:
        """
        :type: int
        """
    @property
    def Skipped(self) -> int:
        """
        :type: int
        """
    @property
    def State(self) -> State:
        """
        :type: State
        """
    @property
    def Total(self) -> int:
        """
        :type: int
        """
    @property
    def Undispatched(self) -> int:
        """
        :type: int
        """
    pass
class State():
    """
    Members:

      Idle

      Queued

      Loading

      Ready

      Failed

      Released
    """
    def __eq__(self, other: object) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: int) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: int) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str:
        """
        :type: str
        """
    @property
    def value(self) -> int:
        """
        :type: int
        """
    Failed: HEngine.RuntimeResourceWarmup.State # value = <State.Failed: 4>
    Idle: HEngine.RuntimeResourceWarmup.State # value = <State.Idle: 0>
    Loading: HEngine.RuntimeResourceWarmup.State # value = <State.Loading: 2>
    Queued: HEngine.RuntimeResourceWarmup.State # value = <State.Queued: 1>
    Ready: HEngine.RuntimeResourceWarmup.State # value = <State.Ready: 3>
    Released: HEngine.RuntimeResourceWarmup.State # value = <State.Released: 5>
    __members__: dict # value = {'Idle': <State.Idle: 0>, 'Queued': <State.Queued: 1>, 'Loading': <State.Loading: 2>, 'Ready': <State.Ready: 3>, 'Failed': <State.Failed: 4>, 'Released': <State.Released: 5>}
    pass
class TriggerProgress():
    @property
    def Dispatched(self) -> int:
        """
        :type: int
        """
    @property
    def Failed(self) -> int:
        """
        :type: int
        """
    @property
    def FailedGroups(self) -> int:
        """
        :type: int
        """
    @property
    def IdleGroups(self) -> int:
        """
        :type: int
        """
    @property
    def Loading(self) -> int:
        """
        :type: int
        """
    @property
    def LoadingGroups(self) -> int:
        """
        :type: int
        """
    @property
    def QueuedGroups(self) -> int:
        """
        :type: int
        """
    @property
    def Ratio(self) -> float:
        """
        :type: float
        """
    @property
    def Ready(self) -> int:
        """
        :type: int
        """
    @property
    def ReadyGroups(self) -> int:
        """
        :type: int
        """
    @property
    def ReleasedGroups(self) -> int:
        """
        :type: int
        """
    @property
    def Skipped(self) -> int:
        """
        :type: int
        """
    @property
    def Total(self) -> int:
        """
        :type: int
        """
    @property
    def TotalGroups(self) -> int:
        """
        :type: int
        """
    @property
    def Undispatched(self) -> int:
        """
        :type: int
        """
    pass
def GetGroupProgress(name: str) -> object:
    pass
def GetTriggerProgress(name: str) -> object:
    pass
def QueueTrigger(name: str) -> int:
    pass
def ReleaseGroup(name: str) -> bool:
    pass
def RethrowFailure(name: str) -> None:
    pass
Failed: HEngine.RuntimeResourceWarmup.State # value = <State.Failed: 4>
Idle: HEngine.RuntimeResourceWarmup.State # value = <State.Idle: 0>
Loading: HEngine.RuntimeResourceWarmup.State # value = <State.Loading: 2>
Queued: HEngine.RuntimeResourceWarmup.State # value = <State.Queued: 1>
Ready: HEngine.RuntimeResourceWarmup.State # value = <State.Ready: 3>
Released: HEngine.RuntimeResourceWarmup.State # value = <State.Released: 5>
