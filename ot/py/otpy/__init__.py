"""Local Windows client for ot_core. No game injection or memory writes."""
from .client import Client, LoaderClient, StateReader

__all__ = ["Client", "LoaderClient", "StateReader"]
